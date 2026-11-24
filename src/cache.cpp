#include "cache.h"

#include <algorithm>
#include <functional>

LRUCacheSegment::LRUCacheSegment(std::size_t capacity) : capacity_(capacity) {}

bool LRUCacheSegment::get(int key, std::string &value, std::uint64_t *version)
{
    std::lock_guard lock(mutex_);
    auto it = positions_.find(key);
    if (it == positions_.end()) {
        return false;
    }
    entries_.splice(entries_.begin(), entries_, it->second);
    value = it->second->value;
    if (version) {
        *version = it->second->version;
    }
    return true;
}

void LRUCacheSegment::put(int key, const std::string &value, std::uint64_t version)
{
    if (capacity_ == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        it->second->value = value;
        it->second->version = version;
        entries_.splice(entries_.begin(), entries_, it->second);
        return;
    }
    entries_.push_front({key, value, version});
    try {
        positions_.emplace(key, entries_.begin());
    } catch (...) {
        entries_.pop_front();
        throw;
    }
    if (entries_.size() > capacity_) {
        positions_.erase(entries_.back().key);
        entries_.pop_back();
    }
}

void LRUCacheSegment::remove(int key)
{
    std::lock_guard lock(mutex_);
    auto it = positions_.find(key);
    if (it != positions_.end()) {
        entries_.erase(it->second);
        positions_.erase(it);
    }
}

LRUCache::LRUCache(std::size_t capacity, std::size_t num_segments)
{
    const std::size_t count = std::max<std::size_t>(1, std::min(capacity, num_segments));
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t segment_capacity = capacity / count + (i < capacity % count);
        segments_.push_back(std::make_unique<LRUCacheSegment>(segment_capacity));
    }
}

LRUCacheSegment &LRUCache::segmentFor(int key)
{
    return *segments_[std::hash<int>{}(key) % segments_.size()];
}

bool LRUCache::get(int key, std::string &value, std::uint64_t *version)
{
    return segmentFor(key).get(key, value, version);
}

void LRUCache::put(int key, const std::string &value, std::uint64_t version)
{
    segmentFor(key).put(key, value, version);
}

void LRUCache::remove(int key)
{
    segmentFor(key).remove(key);
}
