#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool
{
public:
    explicit ThreadPool(std::size_t thread_count);
    ~ThreadPool();

    void submit(const std::function<void()> &task);
    bool trySubmit(std::function<void()> task);
    void stop();
    void waitAllTasks();
    void incrementTasksCount();
    void decrementTasksCount();

private:
    void worker();

    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable drained_;
    std::size_t outstanding_ = 0;
    bool stopped_ = false;
};

#endif
