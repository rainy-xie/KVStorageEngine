// cache_test.cpp
#include "cache.h"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

void cacheTest(LRUCache& cache, int thread_count, int operation_count) {
    std::atomic<int> hit_count(0);
    std::atomic<int> miss_count(0);

    auto start_time = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&cache, operation_count, &hit_count, &miss_count]() {
            for (int j = 0; j < operation_count; ++j) {
                int key = j % 1000;  // 模拟有限的键空间
                std::string value;

                if (cache.get(key, value)) {
                    // 缓存命中
                    ++hit_count;
                } else {
                    // 缓存未命中，插入值
                    cache.put(key, "Value_" + std::to_string(key));
                    ++miss_count;
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

    std::cout << "Cache test completed in " << duration << " ms." << std::endl;
    std::cout << "Cache hits: " << hit_count.load() << std::endl;
    std::cout << "Cache misses: " << miss_count.load() << std::endl;
}

int main() {
    size_t cache_capacity = 1000;
    size_t num_segments = 16;
    LRUCache cache(cache_capacity, num_segments);

    int thread_count = 8;
    int operation_count = 100000;

    cacheTest(cache, thread_count, operation_count);

    return 0;
}
