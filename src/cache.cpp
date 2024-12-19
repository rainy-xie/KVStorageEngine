#include "cache.h"
#include <functional>
#include <memory> // 添加此头文件以使用std::make_unique

// 构造函数
LRUCacheSegment::LRUCacheSegment(size_t capacity) : capacity_(capacity) {}

// 获取缓存中的值
bool LRUCacheSegment::get(int key, std::string &value)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = cache_map_.find(key);
    if (it == cache_map_.end())
    {
        return false;
    }
    // 移动到链表头部
    cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
    value = it->second->second;
    return true;
}

// 添加或更新缓存中的值
void LRUCacheSegment::put(int key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end())
    {
        // 更新值并移动到链表头部
        it->second->second = value;
        cache_list_.splice(cache_list_.begin(), cache_list_, it->second);
    }
    else
    {
        // 如果容量已满，移除最久未使用的项
        if (cache_list_.size() >= capacity_)
        {
            auto last = cache_list_.back();
            cache_map_.erase(last.first);
            cache_list_.pop_back();
        }
        // 插入新项到链表头部
        cache_list_.emplace_front(key, value);
        cache_map_[key] = cache_list_.begin();
    }
}

// 删除缓存中的值
void LRUCacheSegment::remove(int key)
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = cache_map_.find(key);
    if (it != cache_map_.end())
    {
        cache_list_.erase(it->second);
        cache_map_.erase(it);
    }
}

// LRUCache构造函数
LRUCache::LRUCache(size_t capacity, size_t num_segments)
    : num_segments_(num_segments)
{
    size_t segment_capacity = capacity / num_segments;
    if (segment_capacity == 0)
    {
        segment_capacity = 1; 
    }
    for (size_t i = 0; i < num_segments_; ++i)
    {
        segments_.emplace_back(std::make_unique<LRUCacheSegment>(segment_capacity));
    }
}

// 根据键计算段的索引
size_t LRUCache::getSegmentIndex(int key)
{
    return std::hash<int>{}(key) % num_segments_;
}

// 获取缓存中的值
bool LRUCache::get(int key, std::string &value)
{
    size_t index = getSegmentIndex(key);
    return segments_[index]->get(key, value);
}

// 添加或更新缓存中的值
void LRUCache::put(int key, const std::string &value)
{
    size_t index = getSegmentIndex(key);
    segments_[index]->put(key, value);
}

// 删除缓存中的值
void LRUCache::remove(int key)
{
    size_t index = getSegmentIndex(key);
    segments_[index]->remove(key);
}
