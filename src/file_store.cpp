#include "file_store.h"
#include "store_log.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace store_log;

namespace {
std::string readBytes(int fd, U64 offset, U64 length) {
    require(length <= std::string().max_size(), "record exceeds addressable memory");
    std::string data(static_cast<std::size_t>(length), '\0');
    readAt(fd, offset, data.data(), data.size());
    return data;
}

void writeFrame(int fd, U64 offset, U64 version, U32 count, const std::string &body) {
    writeAt(fd, offset, frameHeader(version, count, body));
    writeAt(fd, offset + frame_header_size, body);
}

void confirmPath(int fd, const std::string &path) {
    struct stat opened{}, named{};
    if (::fstat(fd, &opened) < 0 || ::stat(path.c_str(), &named) < 0) ioError("stat database path");
    require(opened.st_dev == named.st_dev && opened.st_ino == named.st_ino,
            "database changed while opening; retry open");
}

void rejectLegacyIndex(const std::string &path) {
    auto index = std::filesystem::symlink_status(path + ".idx");
    require(!std::filesystem::exists(index), "legacy .idx present; use migrateLegacy for the v1 data/index pair");
}

int openDatabase(const std::string &path, bool clean_start) {
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd >= 0 || errno != ENOENT) return fd;

    // Publish a complete initial header. A crash during creation can never leave
    // a half-initialized file at the public database path.
    TempFile initial(path);
    writeAt(initial.fd.get(), 0, fileHeader(0, 0));
    syncFile(initial.fd.get());
    if (!clean_start) rejectLegacyIndex(path);
    if (::link(initial.path.c_str(), path.c_str()) < 0 && errno != EEXIST)
        ioError("publish new database");
    if (::unlink(initial.path.c_str()) == 0) initial.path.clear();
    return ::open(path.c_str(), O_RDWR | O_CLOEXEC);
}
} // namespace

FileStore::FileStore(const std::string &file_path, bool clean_start)
    : path_(std::filesystem::weakly_canonical(std::filesystem::absolute(file_path)).string()) {
    Descriptor file(openDatabase(path_, clean_start));
    lockFile(file.get());
    confirmPath(file.get(), path_);
    fd_ = file.get();
    if (clean_start) {
        std::filesystem::remove(path_ + ".idx");
        truncateFile(fd_, 0);
    } else {
        rejectLegacyIndex(path_);
    }
    if (!fileSize(fd_)) {
        writeAt(fd_, 0, fileHeader(0, 0));
        syncFile(fd_);
    }
    recover();
    // Another creator may have died after publishing the name but before syncing it.
    syncParent(path_);
    gc_thread_ = std::thread(&FileStore::runGC, this);
    file.release();
}

FileStore::~FileStore() {
    {
        std::lock_guard guard(gc_mutex_);
        stopping_ = true;
    }
    gc_wakeup_.notify_one();
    gc_thread_.join();
    ::close(fd_);
}

void FileStore::runGC() {
    std::unique_lock guard(gc_mutex_);
    while (!gc_wakeup_.wait_for(guard, std::chrono::hours(2), [this] { return stopping_; })) {
        guard.unlock();
        try {
            garbageCollect();
        } catch (const std::exception &error) {
            std::cerr << "background compaction: " << error.what() << '\n';
        }
        guard.lock();
    }
}

void FileStore::recover() {
    const U64 size = fileSize(fd_);
    auto header = readBytes(fd_, 0, std::min<U64>(file_header_size, size));
    verifyFileHeader(header);
    version_ = little(header, 8, 8);
    U64 snapshots = little(header, 16, 8);
    require(!snapshots || version_, "invalid snapshot high-water mark");
    end_ = file_header_size;
    while (end_ < size) {
        U64 remaining = size - end_;
        auto bytes = readBytes(fd_, end_, std::min<U64>(frame_header_size, remaining));
        if (bytes.size() < frame_header_size) {
            auto prefix = std::string_view(bytes).substr(0, frame_magic.size());
            require(frame_magic.starts_with(prefix), "invalid trailing frame magic");
            break; // A partial final header is the only frame without a verifiable length.
        }
        auto frame = parseFrameHeader(bytes);
        bool snapshot = snapshots != 0;
        if (snapshot) {
            require(frame.operations == 1 && frame.version <= version_, "invalid snapshot record");
        } else {
            require(version_ != std::numeric_limits<U64>::max() && frame.version == version_ + 1,
                    "invalid batch version sequence");
        }
        if (frame.length > remaining - frame_header_size) break;
        auto body = readBytes(fd_, end_ + frame_header_size, frame.length);
        require(checksum(body) == frame.body_crc, "batch payload checksum mismatch");

        std::size_t pos = 0;
        for (U32 i = 0; i < frame.operations; ++i) {
            require(body.size() - pos >= operation_header_size, "truncated operation in complete batch");
            char type = body[pos];
            int key = keyAt(body, pos + 1);
            U64 length = little(body, pos + 5, 8);
            pos += operation_header_size;
            require(length <= body.size() - pos, "invalid value length in complete batch");
            if (type == 1) {
                require(!snapshot || !index_.contains(key), "duplicate snapshot key");
                auto value = std::string_view(body).substr(pos, static_cast<std::size_t>(length));
                index_[key] = {end_ + frame_header_size + pos, length, frame.version, checksum(value)};
            } else {
                require(type == 2 && !length && !snapshot, "invalid deletion in database");
                index_.erase(key);
            }
            pos += static_cast<std::size_t>(length);
        }
        require(pos == body.size(), "extra bytes in complete batch");
        if (snapshot) --snapshots;
        else version_ = frame.version;
        end_ += frame_header_size + frame.length;
    }
    // An interrupted compaction is never published. A short checkpoint is corruption.
    require(snapshots == 0, "incomplete database snapshot");
    if (end_ < size) {
        truncateFile(fd_, end_);
        syncFile(fd_);
    }
}

std::string FileStore::readValue(const Entry &entry) const {
    auto value = readBytes(fd_, entry.offset, entry.length);
    require(checksum(value) == entry.checksum, "stored value checksum mismatch");
    return value;
}

ReadResult FileStore::read(int key) {
    std::lock_guard guard(mutex_);
    if (!healthy_) return {Status::IOError, {}, 0};
    auto it = index_.find(key);
    if (it == index_.end()) return {Status::NotFound, {}, 0};
    try {
        auto value = readValue(it->second);
        ++read_count_;
        return {Status::Ok, std::move(value), it->second.version};
    } catch (const std::runtime_error &) {
        healthy_ = false;
        return {Status::IOError, {}, 0};
    }
}

std::uint64_t FileStore::versionOf(int key) {
    std::lock_guard guard(mutex_);
    auto it = index_.find(key);
    return it == index_.end() ? 0 : it->second.version;
}

Status FileStore::appendLocked(const std::vector<Mutation> &mutations, U64 &committed_version) {
    committed_version = 0;
    if (!healthy_) return Status::IOError;
    if (mutations.empty()) return Status::Ok;
    if (mutations.size() > std::numeric_limits<U32>::max() || version_ == std::numeric_limits<U64>::max())
        return Status::InvalidArgument;

    const U64 next = version_ + 1;
    std::string body;
    Index changes;
    for (const auto &item : mutations) {
        bool put = item.kind == Mutation::Kind::Put;
        if (!put && (item.kind != Mutation::Kind::Delete || !item.value.empty())) return Status::InvalidArgument;
        auto position = body.size();
        addOperation(body, put, item.key, item.value);
        changes[item.key] = {end_ + frame_header_size + position + operation_header_size,
                             item.value.size(), put ? next : 0, checksum(item.value)};
    }
    // Preallocate index nodes and buckets before persisting any part of the frame.
    // Node transfer after the write can then publish every key without allocating.
    if (index_.empty() || index_.size() + changes.size() > index_.bucket_count())
        index_.reserve(std::max(index_.size() * 2, index_.size() + changes.size()));
    try {
        writeFrame(fd_, end_, next, static_cast<U32>(mutations.size()), body);
    } catch (const std::runtime_error &) {
        try {
            truncateFile(fd_, end_);
            syncFile(fd_);
        } catch (const std::runtime_error &) {
            healthy_ = false;
        }
        return Status::IOError;
    }
    while (!changes.empty()) {
        auto node = changes.extract(changes.begin());
        index_.erase(node.key());
        if (node.mapped().version) index_.insert(std::move(node));
    }
    end_ += frame_header_size + body.size();
    version_ = committed_version = next;
    return Status::Ok;
}

Status FileStore::appendBatch(const std::vector<Mutation> &mutations, U64 &committed_version) {
    std::lock_guard guard(mutex_);
    return appendLocked(mutations, committed_version);
}

Status FileStore::sync() {
    std::lock_guard guard(mutex_);
    if (!healthy_) return Status::IOError;
    try {
        syncFile(fd_);
        return Status::Ok;
    } catch (const std::runtime_error &) {
        healthy_ = false;
        return Status::IOError;
    }
}

bool FileStore::healthy() const {
    return healthy_.load();
}

std::size_t FileStore::getReadCount() const {
    return read_count_.load();
}

bool FileStore::put(int key, const std::string &value) {
    U64 version;
    return appendBatch({Mutation::put(key, value)}, version) == Status::Ok;
}

std::string FileStore::get(int key) {
    return read(key).value;
}

bool FileStore::del(int key) {
    std::lock_guard guard(mutex_);
    U64 version;
    return index_.contains(key) && appendLocked({Mutation::del(key)}, version) == Status::Ok;
}

void FileStore::garbageCollect() {
    std::lock_guard guard(mutex_);
    require(healthy_, "cannot compact a database with an I/O error");
    TempFile replacement(path_);
    lockFile(replacement.fd.get());
    // Preserve permissions when the new inode takes over the old path.
    struct stat info{};
    if (::fstat(fd_, &info) < 0 || ::fchmod(replacement.fd.get(), info.st_mode & 0777) < 0)
        ioError("copy database permissions");
    writeAt(replacement.fd.get(), 0, fileHeader(version_, index_.size()));
    U64 offset = file_header_size;
    Index updated;
    updated.reserve(index_.size());
    for (const auto &[key, entry] : index_) {
        std::string value;
        try {
            value = readValue(entry);
        } catch (const std::runtime_error &) {
            healthy_ = false;
            throw;
        }
        std::string body;
        addOperation(body, true, key, value);
        writeFrame(replacement.fd.get(), offset, entry.version, 1, body);
        updated[key] = {offset + frame_header_size + operation_header_size, entry.length, entry.version, entry.checksum};
        offset += frame_header_size + body.size();
    }
    syncFile(replacement.fd.get());
    if (::rename(replacement.path.c_str(), path_.c_str()) < 0) ioError("publish compacted database");
    replacement.path.clear();
    int previous_fd = fd_;
    fd_ = replacement.fd.release();
    end_ = offset;
    index_.swap(updated);
    ::close(previous_fd);
    try {
        syncParent(path_);
    } catch (...) {
        healthy_ = false;
        throw;
    }
}

void FileStore::migrateLegacy(const std::string &source, const std::string &destination) {
    // v1 persisted the original Linux LP64 struct, including native padding.
    // Parse individual fields so host alignment and uninitialized padding are irrelevant.
    constexpr U64 legacy_entry_size = 32;
    Descriptor data(::open(source.c_str(), O_RDONLY | O_CLOEXEC));
    Descriptor index(::open((source + ".idx").c_str(), O_RDONLY | O_CLOEXEC));
    const U64 data_size = fileSize(data.get());
    const U64 index_size = fileSize(index.get());
    require(index_size >= 8, "invalid legacy index header");
    const U64 count = little(readBytes(index.get(), 0, 8), 0, 8);
    require((index_size - 8) % legacy_entry_size == 0 && count == (index_size - 8) / legacy_entry_size,
            "invalid legacy index length or ABI (fetch Git LFS files before migration)");
    if (std::filesystem::exists(destination)) throw std::runtime_error("migration destination already exists");
    rejectLegacyIndex(destination);
    TempFile replacement(destination);
    U64 offset = file_header_size;
    U64 live = 0;
    auto visit = [&](auto &&visitor) {
        for (U64 i = 0; i < count; ++i) {
            auto row = readBytes(index.get(), 8 + i * legacy_entry_size, legacy_entry_size);
            int key = keyAt(row, 0);
            U64 start = little(row, 8, 8);
            U64 length = little(row, 16, 8);
            unsigned char deleted = static_cast<unsigned char>(row[24]);
            require(deleted <= 1 && start <= data_size && length <= data_size - start,
                    "invalid legacy index range or deletion flag");
            visitor(key, start, length, deleted == 1);
        }
    };
    {
        std::unordered_set<int> seen;
        visit([&](int key, U64 start, U64 length, bool deleted) {
            require(seen.insert(key).second, "duplicate key in legacy index");
            if (deleted) return;
            std::string body;
            addOperation(body, true, key, readBytes(data.get(), start, length));
            writeFrame(replacement.fd.get(), offset, 1, 1, body);
            offset += frame_header_size + body.size();
            ++live;
        });
    }
    writeAt(replacement.fd.get(), 0, fileHeader(live ? 1 : 0, live));
    syncFile(replacement.fd.get());
    // Verify every key and byte using the ordinary v2 recovery and lookup paths.
    {
        FileStore verified(replacement.path);
        visit([&](int key, U64 start, U64 length, bool deleted) {
            ReadResult result = verified.read(key);
            if (deleted) require(result.status == Status::NotFound, "deleted legacy key survived migration");
            else require(result.status == Status::Ok && result.value == readBytes(data.get(), start, length),
                         "legacy migration verification failed");
        });
    }
    // link() publishes exclusively, including if another migration won a race.
    rejectLegacyIndex(destination);
    if (::link(replacement.path.c_str(), destination.c_str()) < 0) ioError("publish migrated database");
    if (::unlink(replacement.path.c_str()) == 0) replacement.path.clear();
    syncParent(destination);
}
