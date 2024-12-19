#include <gtest/gtest.h>
#include "engine.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <filesystem>

static const std::string TEST_DB_FILE = "data/test_db.dat";
static const std::string TEST_DB_IDX_FILE = "data/test_db.dat.idx";

class EngineTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // 清理测试用文件
        if (std::filesystem::exists(TEST_DB_FILE))
        {
            std::filesystem::remove(TEST_DB_FILE);
        }
        if (std::filesystem::exists(TEST_DB_IDX_FILE))
        {
            std::filesystem::remove(TEST_DB_IDX_FILE);
        }
    }

    void TearDown() override
    {
        // 测试结束后清理
        if (std::filesystem::exists(TEST_DB_FILE))
        {
            std::filesystem::remove(TEST_DB_FILE);
        }
        if (std::filesystem::exists(TEST_DB_IDX_FILE))
        {
            std::filesystem::remove(TEST_DB_IDX_FILE);
        }
    }
};

// 基本同步PUT/GET测试
TEST_F(EngineTest, BasicPutGet)
{
    StorageEngine engine(TEST_DB_FILE, 4, 100, 8);
    bool result = engine.put(1, "hello");
    EXPECT_TRUE(result);

    std::string val = engine.get(1);
    EXPECT_EQ(val, "hello");

    // 更新同一键的值
    bool update_result = engine.put(1, "world");
    EXPECT_TRUE(update_result);

    // GET验证更新后的值
    std::string updated_val = engine.get(1);
    EXPECT_EQ(updated_val, "world");
}

// 测试DELETE
TEST_F(EngineTest, BasicDelete)
{
    StorageEngine engine(TEST_DB_FILE, 4, 100, 8);
    engine.put(2, "test");
    std::string val = engine.get(2);
    EXPECT_EQ(val, "test");

    bool del_res = engine.del(2);
    EXPECT_TRUE(del_res);
    val = engine.get(2);
    EXPECT_TRUE(val.empty()); // 已被删除，应无数据
}

// 测试缓存命中
TEST_F(EngineTest, CacheHit)
{
    StorageEngine engine(TEST_DB_FILE, 4, 16, 4);

    // 首次put并get，必然访问文件
    engine.put(100, "cache_value");
    std::string val1 = engine.get(100);
    EXPECT_EQ(val1, "cache_value");

    // 获取FileStore读计数
    size_t reads_after_first_get = engine.getFileStoreReadCount();

    // 再次get同一key，如果缓存正常，FileStore读计数不会改变
    std::string val2 = engine.get(100);
    EXPECT_EQ(val2, "cache_value");

    size_t reads_after_second_get = engine.getFileStoreReadCount();
    EXPECT_EQ(reads_after_first_get, reads_after_second_get);
}

// LRU缓存淘汰行为测试
TEST_F(EngineTest, LRUBehavior)
{
    StorageEngine engine(TEST_DB_FILE, 4, 3, 1); // 线程池大小4，缓存容量3，缓存段数1

    // 初始读取计数
    size_t initial_reads = engine.getFileStoreReadCount();
    EXPECT_EQ(initial_reads, 0);

    // 插入3个键值对
    engine.put(1, "value1");
    engine.put(2, "value2");
    engine.put(3, "value3");

    // 这三个put操作应将数据写入缓存，不应触发FileStore::get
    EXPECT_EQ(engine.getFileStoreReadCount(), initial_reads);

    // 获取所有3个键，应该命中缓存，不触发FileStore::get {3,2,1}
    EXPECT_EQ(engine.get(1), "value1");
    EXPECT_EQ(engine.get(2), "value2");
    EXPECT_EQ(engine.get(3), "value3");

    // 读取计数应保持不变
    EXPECT_EQ(engine.getFileStoreReadCount(), initial_reads);

    // 访问键1，使其成为最近使用 {1,3,2}
    EXPECT_EQ(engine.get(1), "value1");

    // 读取计数仍应保持不变
    EXPECT_EQ(engine.getFileStoreReadCount(), initial_reads);

    // 插入第4个键，缓存容量为3，应淘汰最久未使用的键2 {4,1,3}
    engine.put(4, "value4");

    // 获取键2，应触发FileStore::get，因为键2被淘汰 {2,4,1}
    EXPECT_EQ(engine.get(2), "value2");

    // 读取计数应增加1
    EXPECT_EQ(engine.getFileStoreReadCount(), initial_reads + 1);

    // 再次获取键2，应命中缓存，不触发FileStore::get
    EXPECT_EQ(engine.get(2), "value2");

    // 读取计数应保持不变
    EXPECT_EQ(engine.getFileStoreReadCount(), initial_reads + 1);

    // 获取键3和键4，键4应命中缓存，键3应触发FileStore::get {3,2,4}
    EXPECT_EQ(engine.get(3), "value3");
    EXPECT_EQ(engine.get(4), "value4");

    // 读取计数应增加1
    EXPECT_EQ(engine.getFileStoreReadCount(), initial_reads + 2);
}

// 测试异步操作（asyncPut/asyncGet/asyncDel）
TEST_F(EngineTest, AsyncOperations)
{
    StorageEngine engine(TEST_DB_FILE, 4, 100, 8);

    std::atomic<bool> put_done{false};
    std::atomic<bool> get_done{false};
    std::atomic<bool> del_done{false};

    engine.asyncPut(10, "async_val", [&put_done](bool res)
                    {
        EXPECT_TRUE(res);
        put_done = true; });

    // 等待put完成
    while (!put_done.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    engine.asyncGet(10, [&get_done](std::string val)
                    {
        EXPECT_EQ(val, "async_val");
        get_done = true; });

    while (!get_done.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    engine.asyncDel(10, [&del_done](bool res)
                    {
        EXPECT_TRUE(res);
        del_done = true; });

    while (!del_done.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 最终检查是否删除成功
    auto final_val = engine.get(10);
    EXPECT_TRUE(final_val.empty());
}

// 测试GC逻辑
TEST_F(EngineTest, GarbageCollectTest)
{
    StorageEngine engine(TEST_DB_FILE, 4, 100, 8);

    // 插入多条记录
    for (int i = 0; i < 20; ++i)
    {
        engine.put(i, "value_" + std::to_string(i));
    }

    // 删除部分记录
    for (int i = 0; i < 10; ++i)
    {
        engine.del(i);
    }

    // 调用GC
    engine.garbageCollect();

    // 验证剩余记录
    for (int i = 10; i < 20; ++i)
    {
        std::string val = engine.get(i);
        EXPECT_EQ(val, "value_" + std::to_string(i));
    }

    // 删除的应为空
    for (int i = 0; i < 10; ++i)
    {
        std::string val = engine.get(i);
        EXPECT_TRUE(val.empty());
    }
}

// 简单并发测试
TEST_F(EngineTest, ConcurrentAccess)
{
    StorageEngine engine(TEST_DB_FILE, 8, 100, 16); // 增加缓存段数

    const int N = 1000;
    std::atomic<int> completed_puts{0};
    std::atomic<int> completed_gets{0};

    // 多线程写入
    for (int i = 0; i < N; ++i)
    {
        engine.asyncPut(i, "val_" + std::to_string(i), [&completed_puts](bool res)
                        {
            EXPECT_TRUE(res);
            completed_puts++; });
    }

    // 等待写入全部完成
    while (completed_puts.load() < N)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 多线程异步读
    for (int i = 0; i < N; ++i)
    {
        engine.asyncGet(i, [&completed_gets, i](std::string val)
                        {
            EXPECT_EQ(val, "val_" + std::to_string(i));
            completed_gets++; });
    }

    while (completed_gets.load() < N)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
