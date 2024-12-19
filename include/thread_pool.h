#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

class ThreadPool
{
public:
    explicit ThreadPool(size_t thread_count);
    ~ThreadPool();

    void submit(const std::function<void()> &task);

    void incrementTasksCount();
    void decrementTasksCount();
    void waitAllTasks();

private:
    void worker(); // 工作线程的主循环

    std::vector<std::thread> threads_;        // 工作线程
    std::queue<std::function<void()>> tasks_; // 任务队列

    std::mutex mutex_;           // 保护任务队列的互斥锁
    std::condition_variable cv_; // 用于任务通知的条件变量
    std::atomic<bool> stop_;     // 控制线程池是否停止

    // 等待所有任务完成
    std::mutex tasks_done_mutex_;
    std::condition_variable tasks_done_cv_;
    size_t active_tasks_count_ = 0;
};

#endif // THREAD_POOL_H
