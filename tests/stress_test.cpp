#include <iostream>
#include <thread>
#include <vector>
#include <random>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include "engine.h" 

// 配置参数
static const int NUM_THREADS = 16;                       // 并发线程数
static const size_t KEY_RANGE = 20000000;                // 键空间足够大，减少热点
static const size_t INITIAL_PUT_COUNT = 10000000;        // 至少插入1千万
static const auto MAX_DURATION = std::chrono::hours(12); // 运行不低于12小时
static const std::string TEST_DB_FILE = "data/long_stress_test_db.dat";

// 原子计数统计
std::atomic<size_t> put_success(0);
std::atomic<size_t> put_fail(0);
std::atomic<size_t> get_success(0);
std::atomic<size_t> get_fail(0);
std::atomic<size_t> delete_success(0);
std::atomic<size_t> delete_fail(0);

// 记录期望的键值状态
std::unordered_map<int, std::string> reference_map;
std::mutex ref_mtx;

// 随机数生成
std::random_device rd;
std::mt19937 gen(rd());
std::uniform_int_distribution<> op_dist(1, 3); // 1: put, 2: get, 3: delete
std::uniform_int_distribution<int> key_dist(0, KEY_RANGE - 1);
std::uniform_int_distribution<int> value_dist(0, 999999);

// 控制标志
std::atomic<bool> stop_flag(false);

// 时间控制
auto start_time = std::chrono::steady_clock::now();

// 用于统计周期性输出
std::mutex stats_mtx;
std::condition_variable stats_cv;

void operation_thread(StorageEngine &engine)
{
    // 首先完成INITIAL_PUT_COUNT数据的插入
    // 将这些插入分散到各线程执行
    size_t puts_per_thread = INITIAL_PUT_COUNT / NUM_THREADS + 1;
    size_t inserted = 0;

    // 插入初始数据阶段
    while (!stop_flag.load() && inserted < puts_per_thread)
    {
        int key = key_dist(gen);
        std::string val = "init_val_" + std::to_string(value_dist(gen));
        bool res = engine.put(key, val);
        if (res)
        {
            put_success++;
            {
                std::lock_guard<std::mutex> lock(ref_mtx);
                reference_map[key] = val;
            }
        }
        else
        {
            // key已存在不更新的逻辑会导致fail
            put_fail++;
        }
        inserted++;

        auto now = std::chrono::steady_clock::now();
        if (now - start_time > MAX_DURATION)
        {
            stop_flag.store(true);
            break;
        }
    }

    // 初始插入完成后，转入混合模式
    // 混合模式：在已有数据上随机执行put/get/del
    while (!stop_flag.load())
    {
        int op = op_dist(gen);
        int key = key_dist(gen);

        if (op == 1)
        { // put
            std::string val = "value_" + std::to_string(value_dist(gen));
            bool res = engine.put(key, val);
            if (res)
            {
                put_success++;
                {
                    std::lock_guard<std::mutex> lock(ref_mtx);
                    reference_map[key] = val;
                }
            }
            else
            {
                put_fail++;
            }
        }
        else if (op == 2)
        { // get
            std::string val = engine.get(key);
            bool exists;
            std::string expected;
            {
                std::lock_guard<std::mutex> lock(ref_mtx);
                auto it = reference_map.find(key);
                if (it != reference_map.end())
                {
                    exists = true;
                    expected = it->second;
                }
                else
                {
                    exists = false;
                }
            }
            if (exists)
            {
                // 如果期望存在则val应为expected
                if (val == expected)
                {
                    get_success++;
                }
                else
                {
                    get_fail++;
                }
            }
            else
            {
                // 期望不存在则val应为空
                if (val.empty())
                {
                    get_success++;
                }
                else
                {
                    get_fail++;
                }
            }
        }
        else if (op == 3)
        { // delete
            bool res = engine.del(key);
            if (res)
            {
                delete_success++;
                {
                    std::lock_guard<std::mutex> lock(ref_mtx);
                    reference_map.erase(key);
                }
            }
            else
            {
                delete_fail++;
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (now - start_time > MAX_DURATION)
        {
            stop_flag.store(true);
            break;
        }
    }
}

// 定期打印统计信息的线程
void stats_thread()
{
    using namespace std::chrono;
    const auto report_interval = minutes(10);

    while (!stop_flag.load())
    {
        std::unique_lock<std::mutex> lock(stats_mtx);
        stats_cv.wait_for(lock, report_interval, []
                          { return false; });
        if (stop_flag.load())
            break;

        auto elapsed = duration_cast<seconds>(steady_clock::now() - start_time).count();
        size_t total_ops = put_success.load() + put_fail.load() +
                           get_success.load() + get_fail.load() +
                           delete_success.load() + delete_fail.load();

        double ops_per_sec = total_ops / (elapsed > 0 ? (double)elapsed : 1.0);

        std::cout << "[Stats] Elapsed: " << elapsed << "s | "
                  << "Total Ops: " << total_ops << " | "
                  << "Put(S/F): " << put_success.load() << "/" << put_fail.load() << " | "
                  << "Get(S/F): " << get_success.load() << "/" << get_fail.load() << " | "
                  << "Del(S/F): " << delete_success.load() << "/" << delete_fail.load() << " | "
                  << "Throughput: " << ops_per_sec << " ops/s\n";
        std::fflush(stdout);
    }
}

int main()
{
    using namespace std::filesystem;

    // 清理原数据文件
    if (exists(TEST_DB_FILE))
    {
        remove(TEST_DB_FILE);
    }
    if (exists(TEST_DB_FILE + ".idx"))
    {
        remove(TEST_DB_FILE + ".idx");
    }

    // 初始化引擎
    // 参数根据你的实现进行调整（线程池大小、缓存容量、段数）
    StorageEngine engine(TEST_DB_FILE, 16, 200000, 32);

    start_time = std::chrono::steady_clock::now();

    // 启动统计线程
    std::thread st(stats_thread);

    // 启动工作线程
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i)
    {
        threads.emplace_back(operation_thread, std::ref(engine));
    }

    // 等待时间结束或者操作完成
    for (auto &t : threads)
    {
        t.join();
    }

    // 通知统计线程退出
    stop_flag.store(true);
    stats_cv.notify_all();
    st.join();

    // 统计最终结果
    auto end_time = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end_time - start_time;
    size_t total_ops = put_success.load() + put_fail.load() +
                       get_success.load() + get_fail.load() +
                       delete_success.load() + delete_fail.load();
    double throughput = total_ops / elapsed.count();

    std::cout << "No existing index file found. Starting fresh.\n";
    std::cout << "Stress Test Results:\n";
    std::cout << "Total operations: " << total_ops << "\n";
    std::cout << "Put Success: " << put_success.load() << "\n";
    std::cout << "Put Fail: " << put_fail.load() << "\n";
    std::cout << "Get Success: " << get_success.load() << "\n";
    std::cout << "Get Fail: " << get_fail.load() << "\n";
    std::cout << "Delete Success: " << delete_success.load() << "\n";
    std::cout << "Delete Fail: " << delete_fail.load() << "\n";
    std::cout << "Elapsed Time: " << elapsed.count() << " seconds\n";
    std::cout << "Throughput: " << throughput << " ops/sec\n";

    // 数据一致性验证（抽样检查）
    // 抽样检查100个key的正确性
    int sample_count = 100;
    std::vector<int> sample_keys;
    sample_keys.reserve(sample_count);
    {
        std::lock_guard<std::mutex> lock(ref_mtx);
        for (auto &kv : reference_map)
        {
            sample_keys.push_back(kv.first);
            if ((int)sample_keys.size() >= sample_count)
                break;
        }
    }

    bool consistent = true;
    for (auto k : sample_keys)
    {
        std::string expected;
        {
            std::lock_guard<std::mutex> lock(ref_mtx);
            expected = reference_map[k];
        }
        std::string actual = engine.get(k);
        if (actual != expected)
        {
            std::cerr << "Data inconsistency: key=" << k << ", expected=" << expected << ", got=" << actual << std::endl;
            consistent = false;
        }
    }

    if (consistent)
    {
        std::cout << "Data consistency check (sample) passed.\n";
    }
    else
    {
        std::cout << "Data consistency check (sample) failed.\n";
    }

    return 0;
}
