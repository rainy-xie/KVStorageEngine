#include "thread_pool.h"

#include <cstdio>
#include <stdexcept>
#include <utility>

ThreadPool::ThreadPool(std::size_t thread_count)
{
    if (thread_count == 0) {
        throw std::invalid_argument("thread pool requires at least one worker");
    }
    try {
        for (std::size_t i = 0; i < thread_count; ++i) {
            threads_.emplace_back(&ThreadPool::worker, this);
        }
    } catch (...) {
        stop();
        for (auto &thread : threads_) {
            thread.join();
        }
        throw;
    }
}

ThreadPool::~ThreadPool()
{
    stop();
    for (auto &thread : threads_) {
        thread.join();
    }
}

void ThreadPool::stop()
{
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    ready_.notify_all();
}

bool ThreadPool::trySubmit(std::function<void()> task)
{
    {
        std::lock_guard lock(mutex_);
        if (stopped_) {
            return false;
        }
        tasks_.push(std::move(task));
        ++outstanding_;
    }
    ready_.notify_one();
    return true;
}

void ThreadPool::submit(const std::function<void()> &task)
{
    if (!trySubmit(task)) {
        throw std::runtime_error("thread pool is stopped");
    }
}

void ThreadPool::worker()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (const std::exception &error) {
            std::fprintf(stderr, "thread pool task threw: %s\n", error.what());
        } catch (...) {
            std::fputs("thread pool task threw an unknown exception\n", stderr);
        }
        decrementTasksCount();
    }
}

void ThreadPool::incrementTasksCount()
{
    std::lock_guard lock(mutex_);
    ++outstanding_;
}

void ThreadPool::decrementTasksCount()
{
    std::lock_guard lock(mutex_);
    if (--outstanding_ == 0) {
        drained_.notify_all();
    }
}

void ThreadPool::waitAllTasks()
{
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [this] { return outstanding_ == 0; });
}
