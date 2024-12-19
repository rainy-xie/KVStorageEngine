#include "file_store.h"
#include <iostream>
#include <chrono>
#include <thread>

int main() {
    FileStore store("data/test_storage_parallel.dat", 8); // 创建有 8 个线程的线程池

    // 使用多个线程并发执行 PUT、GET、DEL 操作
    for (int i = 0; i < 100; ++i) {
        store.asyncPut(i, "Value_" + std::to_string(i));
    }

    // 等待片刻以保证 PUT 操作完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    for (int i = 0; i < 100; ++i) {
        store.asyncGet(i, [i](const std::string& value) {
            std::cout << "Key " << i << ": " << value << std::endl;
        });
    }

    // 等待片刻以保证 GET 操作完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    for (int i = 0; i < 50; ++i) {
        store.asyncDel(i, [i](bool success) {
            std::cout << "Delete Key " << i << ": " << (success ? "Success" : "Failed") << std::endl;
        });
    }

    // 再次等待以保证 DEL 操作完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    return 0;
}
