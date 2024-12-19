#ifndef ENGINE_H
#define ENGINE_H

#include "file_store.h"
#include "thread_pool.h"
#include "cache.h"
#include <string>
#include <functional>

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
  StorageEngine(const std::string &storage_file, size_t thread_pool_size = 4, size_t cache_capacity = 100, size_t cache_num_segments = 8);
  ~StorageEngine();
  void stop(); // 用于停止接受新任务

  // 提供公共的垃圾回收接口
  void garbageCollect();

  // 异步接口
  void asyncPut(int key, const std::string &value, std::function<void(bool)> callback);
  void asyncGet(int key, std::function<void(std::string)> callback);
  void asyncDel(int key, std::function<void(bool)> callback);

  // 辅助方法，用于处理缓存逻辑
  bool put(int key, const std::string &value);
  std::string get(int key);
  bool del(int key);

  // 获取 FileStore 的读取计数
  size_t getFileStoreReadCount() const;

private:
  std::atomic<bool> stopped_{false};
  ThreadPool thread_pool_;                // 线程池
  std::unique_ptr<FileStore> file_store_; // 文件存储
  LRUCache cache_;                        // 缓存
};

#endif // ENGINE_H
