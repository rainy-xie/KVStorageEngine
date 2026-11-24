#ifndef ENGINE_H
#define ENGINE_H

#include "cache.h"
#include "file_store.h"
#include "kv_types.h"
#include "thread_pool.h"

#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifdef BUILD_DLL
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __declspec(dllimport)
#endif
#else
#define EXPORT
#endif

class EXPORT StorageEngine
{
public:
    StorageEngine(const std::string &storage_file, std::size_t thread_pool_size = 4,
                  std::size_t cache_capacity = 100, std::size_t cache_num_segments = 8,
                  CommitOptions options = {});
    ~StorageEngine();

    ReadResult read(int key);
    std::vector<ReadResult> multiGet(const std::vector<int> &keys);
    Status putIfVersion(int key, std::uint64_t expected_version, const std::string &value);
    Status writeBatch(const std::vector<Mutation> &mutations);
    CommitStats commitStats() const;

    bool put(int key, const std::string &value);
    std::string get(int key);
    bool del(int key);
    void garbageCollect();
    std::size_t getFileStoreReadCount() const;

    void stop();
    void asyncPut(int key, const std::string &value, std::function<void(bool)> callback);
    void asyncGet(int key, std::function<void(std::string)> callback);
    void asyncDel(int key, std::function<void(bool)> callback);

private:
    enum class Check { None, ExpectedVersion, Exists };
    struct CommitRequest;

    Status commit(const std::vector<Mutation> &mutations, Check check = Check::None,
                  std::uint64_t expected_version = 0);
    void commitGroup(const std::list<CommitRequest *> &group);
    Status appendRequest(CommitRequest &request);
    ReadResult readLocked(int key);

    const CommitOptions options_;
    FileStore file_store_;
    LRUCache cache_;

    // One read lock spans a full multiGet. One write lock spans append, sync and cache publication.
    mutable std::shared_mutex state_mutex_;
    CommitStats stats_;
    bool unavailable_ = false;

    // Writers can queue while a different group is syncing its records.
    std::mutex queue_mutex_;
    std::condition_variable group_full_;
    std::list<CommitRequest *> waiting_;
    bool leader_active_ = false;

    ThreadPool thread_pool_;
};

#endif
