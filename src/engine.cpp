#include "engine.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <stdexcept>

namespace {
CommitOptions checkedOptions(CommitOptions options)
{
    if (options.max_group_size == 0 || options.group_delay.count() < 0) {
        throw std::invalid_argument("invalid commit group size or delay");
    }
    return options;
}
}

struct StorageEngine::CommitRequest {
    const std::vector<Mutation> &mutations;
    Check check;
    std::uint64_t expected_version;
    std::uint64_t committed_version = 0;
    Status status = Status::IOError;
    bool done = false;
    std::condition_variable ready;

    CommitRequest(const std::vector<Mutation> &batch, Check condition, std::uint64_t expected)
        : mutations(batch), check(condition), expected_version(expected) {}
};

StorageEngine::StorageEngine(const std::string &storage_file, std::size_t thread_pool_size,
                             std::size_t cache_capacity, std::size_t cache_num_segments,
                             CommitOptions options)
    : options_(checkedOptions(options)), file_store_(storage_file),
      cache_(cache_capacity, cache_num_segments), thread_pool_(thread_pool_size)
{
}

StorageEngine::~StorageEngine()
{
    stop();
    thread_pool_.waitAllTasks();
}

void StorageEngine::stop()
{
    thread_pool_.stop();
}

void StorageEngine::asyncPut(int key, const std::string &value, std::function<void(bool)> callback)
{
    bool accepted = thread_pool_.trySubmit([this, key, value, callback] {
        bool success = put(key, value);
        if (callback) {
            callback(success);
        }
    });
    if (!accepted && callback) {
        callback(false);
    }
}

void StorageEngine::asyncGet(int key, std::function<void(std::string)> callback)
{
    bool accepted = thread_pool_.trySubmit([this, key, callback] {
        auto value = get(key);
        if (callback) {
            callback(std::move(value));
        }
    });
    if (!accepted && callback) {
        callback({});
    }
}

void StorageEngine::asyncDel(int key, std::function<void(bool)> callback)
{
    bool accepted = thread_pool_.trySubmit([this, key, callback] {
        bool success = del(key);
        if (callback) {
            callback(success);
        }
    });
    if (!accepted && callback) {
        callback(false);
    }
}

bool StorageEngine::put(int key, const std::string &value)
{
    return writeBatch({Mutation::put(key, value)}) == Status::Ok;
}

std::string StorageEngine::get(int key)
{
    return read(key).value;
}

bool StorageEngine::del(int key)
{
    return commit({Mutation::del(key)}, Check::Exists) == Status::Ok;
}

ReadResult StorageEngine::readLocked(int key)
{
    if (unavailable_ || !file_store_.healthy()) {
        return {Status::IOError, {}, 0};
    }
    ReadResult result{Status::Ok, {}, 0};
    if (cache_.get(key, result.value, &result.version)) {
        return result;
    }
    result = file_store_.read(key);
    if (result.status == Status::Ok) {
        cache_.put(key, result.value, result.version);
    }
    return result;
}

ReadResult StorageEngine::read(int key)
{
    std::shared_lock lock(state_mutex_);
    return readLocked(key);
}

std::vector<ReadResult> StorageEngine::multiGet(const std::vector<int> &keys)
{
    std::vector<ReadResult> results;
    results.reserve(keys.size());
    std::shared_lock lock(state_mutex_);
    for (int key : keys) {
        results.push_back(readLocked(key));
    }
    return results;
}

Status StorageEngine::putIfVersion(int key, std::uint64_t expected_version, const std::string &value)
{
    return commit({Mutation::put(key, value)}, Check::ExpectedVersion, expected_version);
}

Status StorageEngine::writeBatch(const std::vector<Mutation> &mutations)
{
    if (mutations.empty()) {
        return Status::Ok;
    }
    return commit(mutations);
}

Status StorageEngine::commit(const std::vector<Mutation> &mutations, Check check,
                             std::uint64_t expected_version)
{
    CommitRequest request(mutations, check, expected_version);
    const auto queued_at = std::chrono::steady_clock::now();
    std::unique_lock lock(queue_mutex_);
    waiting_.push_back(&request);
    if (waiting_.size() == options_.max_group_size) {
        group_full_.notify_one();
    }
    request.ready.wait(lock, [&] {
        return request.done || (!leader_active_ && waiting_.front() == &request);
    });
    if (request.done) {
        return request.status;
    }

    leader_active_ = true;
    if (options_.group_delay.count() > 0) {
        group_full_.wait_until(lock, queued_at + options_.group_delay, [&] {
            return waiting_.size() >= options_.max_group_size;
        });
    }

    std::list<CommitRequest *> group;
    auto group_size = std::min(waiting_.size(), options_.max_group_size);
    auto group_end = std::next(waiting_.begin(), group_size);
    group.splice(group.end(), waiting_, waiting_.begin(), group_end);
    lock.unlock();
    commitGroup(group);
    lock.lock();

    for (auto *member : group) {
        member->done = true;
        member->ready.notify_one();
    }
    leader_active_ = false;
    if (!waiting_.empty()) {
        waiting_.front()->ready.notify_one();
    }
    return request.status;
}

Status StorageEngine::appendRequest(CommitRequest &request)
{
    if (request.check != Check::None) {
        int key = request.mutations.front().key;
        auto version = file_store_.versionOf(key);
        if (request.check == Check::ExpectedVersion && version != request.expected_version) {
            return Status::Conflict;
        }
        if (request.check == Check::Exists && version == 0) {
            return Status::NotFound;
        }
    }
    return file_store_.appendBatch(request.mutations, request.committed_version);
}

void StorageEngine::commitGroup(const std::list<CommitRequest *> &group)
{
    std::unique_lock lock(state_mutex_);
    std::size_t appended = 0;
    try {
        for (auto *request : group) {
            if (unavailable_ || !file_store_.healthy()) {
                unavailable_ = true;
                break;
            }
            request->status = appendRequest(*request);
            if (request->status == Status::IOError) {
                unavailable_ = true;
                break;
            }
            if (request->status == Status::Ok) {
                ++appended;
            }
        }

        if (!unavailable_ && appended != 0 && options_.durable) {
            ++stats_.sync_calls;
            unavailable_ = file_store_.sync() != Status::Ok;
        }

        if (!unavailable_) {
            for (auto *request : group) {
                if (request->status != Status::Ok) {
                    continue;
                }
                for (const auto &mutation : request->mutations) {
                    if (mutation.kind == Mutation::Kind::Put) {
                        cache_.put(mutation.key, mutation.value, request->committed_version);
                    } else {
                        cache_.remove(mutation.key);
                    }
                }
            }
            stats_.committed_requests += appended;
            return;
        }
    } catch (...) {
        // An exception after appending leaves the result of the current group uncertain.
        unavailable_ = true;
    }
    for (auto *request : group) {
        request->status = Status::IOError;
    }
}

CommitStats StorageEngine::commitStats() const
{
    std::shared_lock lock(state_mutex_);
    return stats_;
}

std::size_t StorageEngine::getFileStoreReadCount() const
{
    return file_store_.getReadCount();
}

void StorageEngine::garbageCollect()
{
    std::unique_lock lock(state_mutex_);
    if (!unavailable_) {
        file_store_.garbageCollect();
    }
}
