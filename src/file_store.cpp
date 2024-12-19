#include "file_store.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>

// 索引文件存储路径
#define INDEX_FILE_SUFFIX ".idx"

FileStore::FileStore(const std::string &file_path, bool clean_start)
    : file_path_(file_path), file_size_(0), stop_gc_thread_(false), read_count_(0)
{
    if (clean_start)
    {
        if (std::ifstream(file_path_))
        {
            std::remove(file_path_.c_str());
        }
        std::string index_file_path = file_path_ + INDEX_FILE_SUFFIX;
        if (std::ifstream(index_file_path))
        {
            std::remove(index_file_path.c_str());
        }
    }

    // 若数据文件不存在则创建空文件
    if (!std::ifstream(file_path_))
    {
        std::ofstream create_file(file_path_, std::ios::binary);
        create_file.close();
    }

    {
        std::lock_guard<std::mutex> lock(file_mtx_);
        file_.open(file_path_, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_)
        {
            std::cerr << "Failed to open data file: " << file_path_ << std::endl;
        }
    }

    // 加载索引
    loadIndex();

    // 启动GC线程
    startGCThread();
}

FileStore::~FileStore()
{
    stop_gc_thread_ = true; // 停止垃圾回收线程

    if (gc_thread_.joinable())
    {
        gc_thread_.join(); // 等待线程退出
    }

    if (file_.is_open())
    {
        file_.close();
    }

    saveIndex();
}

// 启动垃圾回收后台线程
void FileStore::startGCThread()
{
    if (gc_thread_.joinable())
    {
        return; // 如果线程已经启动，则直接返回
    }

    gc_thread_ = std::thread([this]()
                             {
        while (!stop_gc_thread_) {
            std::this_thread::sleep_for(std::chrono::hours(2));
            garbageCollect();
        } });
}

size_t FileStore::getReadCount() const
{
    return read_count_;
}

// 同步方法 put
bool FileStore::put(int key, const std::string &value)
{
    // 直接使用独占锁
    std::unique_lock<std::shared_mutex> index_lock(index_mtx_);

    // 写入文件数据
    size_t offset;
    {
        std::lock_guard<std::mutex> file_lock(file_mtx_);
        offset = file_size_;
        file_.seekp(file_size_, std::ios::beg);
        file_.write(value.c_str(), value.size());
        if (!file_)
        {
            std::cerr << "Failed to write to file." << std::endl;
            return false;
        }
        file_.flush();
        file_size_ += value.size();
    }

    // 更新索引
    index_[key] = ObjectMeta{key, offset, value.size(), false};

    return true;
}

// 同步方法 get
std::string FileStore::get(int key)
{
    // 查找索引
    std::shared_lock<std::shared_mutex> index_lock(index_mtx_);
    auto it = index_.find(key);
    if (it == index_.end() || it->second.deleted)
    {
        return ""; // Key 未找到或已被删除
    }

    const ObjectMeta &meta = it->second;

    // 从文件读取数据
    std::string value;
    value.resize(meta.size);
    {
        std::lock_guard<std::mutex> file_lock(file_mtx_);
        file_.seekg(meta.offset, std::ios::beg);
        file_.read(&value[0], meta.size);
        if (!file_)
        {
            std::cerr << "Failed to read from file." << std::endl;
            return "";
        }
        read_count_++;
    }

    return value;
}

// 同步方法 del
bool FileStore::del(int key)
{
    // 从索引中查找
    std::unique_lock<std::shared_mutex> index_lock(index_mtx_);
    auto it = index_.find(key);
    if (it == index_.end() || it->second.deleted)
    {
        return false; // Key 未找到或已被删除
    }

    // 标记为已删除
    it->second.deleted = true;

    return true;
}

// 定期清理无效数据
void FileStore::garbageCollect()
{
    // std::cerr << "Start garbageCollect...." << std::endl;
    // printFileContext();

    // 获取独占锁，防止其他操作
    std::unique_lock<std::shared_mutex>
        lock(index_mtx_);

    std::vector<ObjectMeta> live_objects;

    // 遍历索引表，保留没有被删除的对象
    for (auto &entry : index_)
    {
        const ObjectMeta &meta = entry.second;
        if (!meta.deleted)
        {
            live_objects.push_back(meta);
        }
    }

    // 压缩文件，只保留有效对象
    compactFile(live_objects);

    // std::cerr << "After garbageCollect...." << std::endl;
    // printFileContext();
}

// 压缩文件，移除已删除对象的存储
void FileStore::compactFile(const std::vector<ObjectMeta> &live_objects)
{
    std::lock_guard<std::mutex> lock(file_mtx_);

    std::fstream temp_file(file_path_ + ".tmp", std::ios::out | std::ios::binary);
    if (!temp_file)
    {
        std::cerr << "Failed to create temporary file for compacting." << std::endl;
        return;
    }

    size_t new_offset = 0;
    std::unordered_map<int, ObjectMeta> new_index;

    // 将有效数据写入新的文件
    for (const auto &meta : live_objects)
    {
        std::string data;
        data.resize(meta.size);

        // 读取旧文件中的有效数据
        file_.seekg(meta.offset, std::ios::beg);
        file_.read(&data[0], meta.size);

        // 将数据写入新文件
        temp_file.write(data.c_str(), data.size());

        // 更新索引
        ObjectMeta new_meta = meta;
        new_meta.offset = new_offset; // 更新偏移量

        new_index[meta.key] = new_meta;
        new_offset += data.size();
    }

    // 关闭临时文件
    temp_file.close();

    // 替换原文件
    file_.close();
    std::rename((file_path_ + ".tmp").c_str(), file_path_.c_str());

    // 重新打开file_：
    file_.open(file_path_, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open())
    {
        std::cerr << "Failed to reopen data file after compaction." << std::endl;
    }

    // 更新索引
    index_ = std::move(new_index);
    file_size_ = new_offset; // 更新文件大小
}

void FileStore::loadIndex()
{
    std::string index_file_path = file_path_ + INDEX_FILE_SUFFIX;
    std::ifstream index_file(index_file_path, std::ios::in | std::ios::binary);

    if (!index_file)
    {
        std::cerr << "No existing index file found. Starting fresh." << std::endl;
        return; // 如果没有索引文件，启动时不会加载任何元数据
    }

    // 从索引文件中读取所有元数据
    size_t index_size;
    index_file.read(reinterpret_cast<char *>(&index_size), sizeof(index_size));

    for (size_t i = 0; i < index_size; ++i)
    {
        ObjectMeta meta;
        index_file.read(reinterpret_cast<char *>(&meta), sizeof(ObjectMeta));
        index_[meta.key] = meta;
    }

    // 计算文件大小：遍历索引，找出最大的偏移量 + 对应的大小
    file_size_ = 0;
    for (const auto &pair : index_)
    {
        const ObjectMeta &meta = pair.second;
        size_t end_position = meta.offset + meta.size;
        if (end_position > file_size_)
        {
            file_size_ = end_position;
        }
    }

    index_file.close();
}

void FileStore::saveIndex()
{
    std::string index_file_path = file_path_ + INDEX_FILE_SUFFIX;
    std::ofstream index_file(index_file_path, std::ios::out | std::ios::binary);

    if (!index_file)
    {
        std::cerr << "Failed to save index to file!" << std::endl;
        return;
    }

    // 保存当前索引大小
    size_t index_size = index_.size();
    index_file.write(reinterpret_cast<const char *>(&index_size), sizeof(index_size));

    // 保存所有对象的元数据
    for (const auto &pair : index_)
    {
        const ObjectMeta &meta = pair.second;
        index_file.write(reinterpret_cast<const char *>(&meta), sizeof(ObjectMeta));
    }

    index_file.close();
}

void FileStore::printFileContext()
{
    std::ifstream file(file_path_);
    if (!file.is_open())
    {
        std::cerr << "Failed to open: " << file_path_ << std::endl;
        return;
    }

    std::string line;
    std::cout << "[File context] " << std::endl;
    while (std::getline(file, line))
    {
        std::cout << line << std::endl;
    }

    file.close();
}
