#ifndef CACHE_H
#define CACHE_H

#include <unordered_map>
#include <list>
#include <mutex>
#include <vector>
#include <memory>

// 单个缓存段，使用LRU策略
class LRUCacheSegment
{
public:
    LRUCacheSegment(size_t capacity);
    ~LRUCacheSegment() = default;

    bool get(int key, std::string &value);
    void put(int key, const std::string &value);
    void remove(int key);

private:
    size_t capacity_;
    std::list<std::pair<int, std::string>> cache_list_;
    std::unordered_map<int, std::list<std::pair<int, std::string>>::iterator> cache_map_;
    std::mutex mtx_;
};

// 分段缓存
class LRUCache
{
public:
    LRUCache(size_t capacity, size_t num_segments);
    ~LRUCache() = default;

    bool get(int key, std::string &value);
    void put(int key, const std::string &value);
    void remove(int key);

private:
    size_t num_segments_;
    std::vector<std::unique_ptr<LRUCacheSegment>> segments_;

    size_t getSegmentIndex(int key);
};

#endif // CACHE_H
