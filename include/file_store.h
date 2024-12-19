#ifndef FILE_STORE_H
#define FILE_STORE_H

#include <string>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <fstream>
#include <vector>
#include <atomic>
#include <thread>

// 对象元数据
struct ObjectMeta
{
    int key;              // 对象的Key
    size_t offset;        // 数据在文件中的偏移量
    size_t size;          // 数据大小
    bool deleted = false; // 标记该对象是否已删除
};

// 文件存储引擎类
class FileStore
{
public:
    FileStore(const std::string &file_path, bool clean_start = false);
    ~FileStore();

    // 删除复制构造函数和复制赋值运算符
    FileStore(const FileStore &) = delete;
    FileStore &operator=(const FileStore &) = delete;

    // 同步操作
    bool put(int key, const std::string &value);
    std::string get(int key);
    bool del(int key);

    size_t getReadCount() const;

    // 垃圾回收
    void garbageCollect();

private:
    std::string file_path_;                     // 文件路径
    std::fstream file_;                         // 文件流
    std::unordered_map<int, ObjectMeta> index_; // 索引表 (Key -> ObjectMeta)
    std::shared_mutex index_mtx_;               // 用于保护索引的读写锁
    std::mutex file_mtx_;                       // 用于保护文件操作的互斥锁
    size_t file_size_;                          // 文件当前大小 (用于定位新写入数据的偏移量)
    std::atomic<bool> stop_gc_thread_;          // 标记垃圾回收线程是否停止
    std::thread gc_thread_;
    std::atomic<size_t> read_count_; // get访问底层存储的计数

    // 启动垃圾回收线程
    void startGCThread();

    // 索引管理
    void loadIndex();        // 从文件加载索引
    void saveIndex();        // 保存索引到文件
    void printFileContext(); // 打印data文件的内容

    // 压缩文件
    void compactFile(const std::vector<ObjectMeta> &live_objects);
};

#endif // FILE_STORE_H
