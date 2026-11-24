#include "store_log.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace store_log {
namespace {
static_assert(sizeof(int) == 4);
constexpr auto crc_table = [] {
    std::array<U32, 256> table{};
    for (U32 n = 0; n < table.size(); ++n) {
        U32 crc = n;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
        table[n] = crc;
    }
    return table;
}();

void appendLittle(std::string &out, U64 value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
        out.push_back(static_cast<char>(value & 0xff));
        value >>= 8;
    }
}

void checkRange(U64 offset, U64 length) {
    constexpr U64 limit = std::numeric_limits<off_t>::max();
    require(offset <= limit && length <= limit - offset, "file offset exceeds platform limit");
}
} // namespace

[[noreturn]] void ioError(const char *operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

Descriptor::Descriptor(int fd) : fd_(fd) {
    if (fd_ == -1) ioError("open file");
}

Descriptor::~Descriptor() {
    if (fd_ >= 0) ::close(fd_);
}

int Descriptor::release() {
    int fd = fd_;
    fd_ = -1;
    return fd;
}

TempFile::TempFile(const std::string &target)
    : path(target + ".tmp.XXXXXX"), fd(::mkostemp(path.data(), O_CLOEXEC)) {}

TempFile::~TempFile() {
    if (!path.empty()) ::unlink(path.c_str());
}

void lockFile(int fd) {
    int result;
    do { result = ::flock(fd, LOCK_EX | LOCK_NB); } while (result < 0 && errno == EINTR);
    if (result < 0) ioError("lock database for exclusive use");
}

void syncFile(int fd) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) ioError("fsync");
}

void syncParent(const std::string &path) {
    auto parent = std::filesystem::path(path).parent_path();
    Descriptor directory(::open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    syncFile(directory.get());
}

void truncateFile(int fd, U64 length) {
    int result;
    do { result = ::ftruncate(fd, static_cast<off_t>(length)); } while (result < 0 && errno == EINTR);
    if (result < 0) ioError("truncate file");
}

U64 fileSize(int fd) {
    struct stat info{};
    if (::fstat(fd, &info) < 0) ioError("stat open file");
    require(S_ISREG(info.st_mode) && info.st_size >= 0, "database must be a regular file");
    return static_cast<U64>(info.st_size);
}

void readAt(int fd, U64 offset, char *buffer, std::size_t size) {
    checkRange(offset, size);
    while (size) {
        ssize_t n = ::pread(fd, buffer, std::min<std::size_t>(size, 1U << 30), static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ioError("pread");
        require(n > 0, "unexpected end of database");
        buffer += n;
        offset += n;
        size -= n;
    }
}

void writeAt(int fd, U64 offset, std::string_view bytes) {
    checkRange(offset, bytes.size());
    while (!bytes.empty()) {
        ssize_t n = ::pwrite(fd, bytes.data(), std::min<std::size_t>(bytes.size(), 1U << 30),
                             static_cast<off_t>(offset));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) ioError("pwrite");
        require(n > 0, "zero-length write to database");
        bytes.remove_prefix(static_cast<std::size_t>(n));
        offset += n;
    }
}

U32 checksum(std::string_view bytes) {
    U32 crc = ~0U;
    for (unsigned char byte : bytes) crc = crc_table[(crc ^ byte) & 0xff] ^ (crc >> 8);
    return ~crc;
}

U64 little(std::string_view bytes, std::size_t offset, unsigned width) {
    U64 value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= U64(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return value;
}

std::string fileHeader(U64 highwater, U64 snapshots) {
    std::string result(file_magic);
    appendLittle(result, highwater, 8);
    appendLittle(result, snapshots, 8);
    appendLittle(result, checksum(result), 4);
    return result;
}

void verifyFileHeader(std::string_view header) {
    require(header.starts_with(file_magic), "unknown database format; use migrateLegacy for a v1 data/index pair");
    require(header.size() == file_header_size, "truncated database file header");
    require(little(header, 24, 4) == checksum(header.substr(0, 24)), "database header checksum mismatch");
}

std::string frameHeader(U64 version, U32 operations, std::string_view body) {
    std::string header(frame_magic);
    appendLittle(header, body.size(), 8);
    appendLittle(header, version, 8);
    appendLittle(header, operations, 4);
    appendLittle(header, checksum(body), 4);
    appendLittle(header, checksum(header), 4);
    return header;
}

FrameHeader parseFrameHeader(std::string_view header) {
    require(header.size() == frame_header_size && header.starts_with(frame_magic), "invalid batch frame header");
    require(little(header, 28, 4) == checksum(header.substr(0, 28)), "batch header checksum mismatch");
    FrameHeader result{little(header, 4, 8), little(header, 12, 8),
                       static_cast<U32>(little(header, 20, 4)), static_cast<U32>(little(header, 24, 4))};
    require(result.operations && result.version && result.length / operation_header_size >= result.operations,
            "invalid batch header fields");
    return result;
}

void addOperation(std::string &body, bool is_put, int key, std::string_view value) {
    body.push_back(is_put ? '\1' : '\2');
    appendLittle(body, static_cast<U32>(key), 4);
    appendLittle(body, value.size(), 8);
    body.append(value);
}

int keyAt(std::string_view bytes, std::size_t offset) {
    return std::bit_cast<std::int32_t>(static_cast<U32>(little(bytes, offset, 4)));
}
} // namespace store_log
