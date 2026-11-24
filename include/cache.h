#ifndef CACHE_H
#define CACHE_H

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class LRUCacheSegment
{
public:
    explicit LRUCacheSegment(std::size_t capacity);
    bool get(int key, std::string &value, std::uint64_t *version = nullptr);
    void put(int key, const std::string &value, std::uint64_t version = 0);
    void remove(int key);

private:
    struct Entry {
        int key;
        std::string value;
        std::uint64_t version;
    };

    std::size_t capacity_;
    std::list<Entry> entries_;
    std::unordered_map<int, std::list<Entry>::iterator> positions_;
    std::mutex mutex_;
};

class LRUCache
{
public:
    LRUCache(std::size_t capacity, std::size_t num_segments);
    bool get(int key, std::string &value, std::uint64_t *version = nullptr);
    void put(int key, const std::string &value, std::uint64_t version = 0);
    void remove(int key);

private:
    std::vector<std::unique_ptr<LRUCacheSegment>> segments_;
    LRUCacheSegment &segmentFor(int key);
};

#endif
