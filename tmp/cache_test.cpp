#include "cache.h"
#include <iostream>

int main() {
    // 创建 LRUCache 对象，容量设为 3
    LRUCache cache(3);

    // 插入初始数据到缓存中
    cache.put(1, "Data_1");
    std::cout << "Inserted Key 1" << std::endl;

    cache.put(2, "Data_2");
    std::cout << "Inserted Key 2" << std::endl;

    cache.put(3, "Data_3");
    std::cout << "Inserted Key 3" << std::endl;

    // 缓存应为 [3, 2, 1]
    cache.put(4, "Data_4");  // 此时应该淘汰 Key 1
    std::cout << "Inserted Key 4, expected eviction of Key 1" << std::endl;

    // 检查是否成功淘汰了 Key 1
    std::string result;
    if (!cache.get(1, result)) {
        std::cout << "Key 1 has been evicted from cache as expected." << std::endl;
    } else {
        std::cout << "Key 1 is still in cache (unexpected): " << result << std::endl;
    }

    // 检查缓存中其他键是否存在
    if (cache.get(2, result)) {
        std::cout << "Getting Key 2 (expect Data_2): " << result << std::endl;
    }
    if (cache.get(3, result)) {
        std::cout << "Getting Key 3 (expect Data_3): " << result << std::endl;
    }
    if (cache.get(4, result)) {
        std::cout << "Getting Key 4 (expect Data_4): " << result << std::endl;
    }

    // 使用 Key 2，确保 Key 2 成为最近使用的
    cache.get(2, result);
    std::cout << "Accessed Key 2, making it the most recently used." << std::endl;

    // 插入新数据，缓存应为 [2, 4, 5]，此时应该淘汰 Key 3
    cache.put(5, "Data_5");
    std::cout << "Inserted Key 5, expected eviction of Key 3" << std::endl;

    // 检查是否成功淘汰了 Key 3
    if (!cache.get(3, result)) {
        std::cout << "Key 3 has been evicted from cache as expected." << std::endl;
    } else {
        std::cout << "Key 3 is still in cache (unexpected): " << result << std::endl;
    }

    // 检查其他数据是否存在
    if (cache.get(2, result)) {
        std::cout << "Getting Key 2 (expect Data_2): " << result << std::endl;
    }
    if (cache.get(4, result)) {
        std::cout << "Getting Key 4 (expect Data_4): " << result << std::endl;
    }
    if (cache.get(5, result)) {
        std::cout << "Getting Key 5 (expect Data_5): " << result << std::endl;
    }

    return 0;
}
