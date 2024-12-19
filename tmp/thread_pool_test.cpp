#include "thread_pool.h"
#include <iostream>
#include <chrono>

int main() {
    ThreadPool pool(4);

    for (int i = 0; i < 10; ++i) {
        pool.submit([i] {
            std::cout << "Task " << i << " is running on thread " 
                      << std::this_thread::get_id() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(2)); // 等待所有任务完成
    return 0;
}


// #include "thread_pool.h"
// #include <iostream>
// #include <atomic>
// #include <chrono>

// int main() {
//     ThreadPool pool(4);
//     std::atomic<int> counter = 0;

//     for (int i = 0; i < 100; ++i) {
//         pool.submit([&counter, i] {
//             ++counter;
//             std::cout << "Task " << i << " is running on thread "
//                       << std::this_thread::get_id() << std::endl;
//             std::this_thread::sleep_for(std::chrono::milliseconds(10));
//         });
//     }

//     std::this_thread::sleep_for(std::chrono::seconds(2)); // 等待所有任务完成
//     std::cout << "Final counter value: " << counter << std::endl;
//     return 0;
// }
