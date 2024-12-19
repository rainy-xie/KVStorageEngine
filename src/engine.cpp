// engine.cpp
#include "engine.h"
#include <iostream>

StorageEngine::StorageEngine(const std::string &storage_file, size_t thread_pool_size, size_t cache_capacity, size_t cache_num_segments)
    : thread_pool_(thread_pool_size), file_store_(std::make_unique<FileStore>(storage_file)), cache_(cache_capacity, cache_num_segments)
{
}

StorageEngine::~StorageEngine()
{
    stop();                      // 不再提交新任务
    thread_pool_.waitAllTasks(); // 等待所有已提交任务执行完毕
}

void StorageEngine::stop()
{
    stopped_ = true;
}

// 异步方法
void StorageEngine::asyncPut(int key, const std::string &value, std::function<void(bool)> callback)
{
    if (stopped_)
    {
        if (callback)
            callback(false);
        return;
    }
    thread_pool_.submit([this, key, value, callback]()
                        {
        bool success = put(key, value);
        if (callback) {
            callback(success);
        } });
}

void StorageEngine::asyncGet(int key, std::function<void(std::string)> callback)
{
    if (stopped_)
    {
        return;
    }
    thread_pool_.submit([this, key, callback]()
                        {
        std::string value = get(key);
        if (callback) {
            callback(value);
        } });
}

void StorageEngine::asyncDel(int key, std::function<void(bool)> callback)
{
    if (stopped_)
    {
        if (callback)
            callback(false);
        return;
    }
    thread_pool_.submit([this, key, callback]()
                        {
        bool success = del(key);
        if (callback) {
            callback(success);
        } });
}

bool StorageEngine::put(int key, const std::string &value)
{
    // 调用底层存储
    if (!file_store_->put(key, value))
    {
        return false; // 存储失败
    }

    // 更新缓存
    cache_.put(key, value);
    return true;
}

std::string StorageEngine::get(int key)
{
    std::string value;
    if (cache_.get(key, value))
    {
        return value; // 缓存命中
    }
    // 缓存未命中，访问底层存储
    value = file_store_->get(key);
    if (!value.empty())
    {
        cache_.put(key, value); // 更新缓存
    }
    return value;
}

bool StorageEngine::del(int key)
{
    cache_.remove(key); // 从缓存中删除
    return file_store_->del(key);
}

size_t StorageEngine::getFileStoreReadCount() const
{
    return file_store_->getReadCount();
}

void StorageEngine::garbageCollect()
{
    file_store_->garbageCollect();
}