#include "engine.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <mutex>

// 全局互斥锁用于保护标准输出
std::mutex cout_mutex;

int main()
{
    StorageEngine engine("data/test_engine_01.dat", 4, 100);

    // 异步写入
    engine.asyncPut(1, "value1", [](bool success)
                    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        if (success) {
            std::cout << "Put key 1 successful." << std::endl;
        } else {
            std::cout << "Put key 1 failed." << std::endl;
        } });

    // 异步读取
    engine.asyncGet(1, [](std::string value)
                    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        if (!value.empty()) {
            std::cout << "Get key 1: " << value << std::endl;
        } else {
            std::cout << "Key 1 not found." << std::endl;
        } });

    // 异步删除
    engine.asyncDel(1, [](bool success)
                    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        if (success) {
            std::cout << "Delete key 1 successful." << std::endl;
        } else {
            std::cout << "Delete key 1 failed." << std::endl;
        } });

    // 再次尝试读取已删除的键
    engine.asyncGet(1, [](std::string value)
                    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        if (!value.empty()) {
            std::cout << "Get key 1: " << value << std::endl;
        } else {
            std::cout << "Key 1 not found after deletion." << std::endl;
        } });

    // 等待一段时间以确保所有异步操作完成
    std::this_thread::sleep_for(std::chrono::seconds(3));

    return 0;
}
