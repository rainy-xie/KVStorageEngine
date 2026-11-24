#ifndef FILE_STORE_H
#define FILE_STORE_H

#include "kv_types.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class FileStore
{
public:
    explicit FileStore(const std::string &file_path, bool clean_start = false);
    ~FileStore();
    FileStore(const FileStore &) = delete;
    FileStore &operator=(const FileStore &) = delete;

    bool put(int key, const std::string &value);
    std::string get(int key);
    bool del(int key);

    ReadResult read(int key);
    std::uint64_t versionOf(int key);
    Status appendBatch(const std::vector<Mutation> &mutations, std::uint64_t &committed_version);
    Status sync();
    bool healthy() const;
    std::size_t getReadCount() const;
    void garbageCollect();

    // Reads the original 64-bit little-endian .dat/.idx pair; destination must be new.
    static void migrateLegacy(const std::string &source, const std::string &destination);

private:
    struct Entry {
        std::uint64_t offset;
        std::uint64_t length;
        std::uint64_t version;
        std::uint32_t checksum;
    };
    using Index = std::unordered_map<int, Entry>;

    void recover();
    Status appendLocked(const std::vector<Mutation> &mutations, std::uint64_t &committed_version);
    std::string readValue(const Entry &entry) const;
    void runGC();

    std::string path_;
    int fd_ = -1;
    std::uint64_t end_ = 0;
    std::uint64_t version_ = 0;
    Index index_;
    std::mutex mutex_;
    std::atomic<bool> healthy_{true};
    std::atomic<std::size_t> read_count_{0};

    std::mutex gc_mutex_;
    std::condition_variable gc_wakeup_;
    bool stopping_ = false;
    std::thread gc_thread_;
};

#endif
