#include "thread_pool.h"

ThreadPool::ThreadPool(size_t thread_count) : stop_(false)
{
    for (size_t i = 0; i < thread_count; ++i)
    {
        threads_.emplace_back(&ThreadPool::worker, this);
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stop_ = true;
        cv_.notify_all(); // 通知所有线程退出
    }

    for (std::thread &thread : threads_)
    {
        if (thread.joinable())
        {
            thread.join(); // 等待线程退出
        }
    }
}

void ThreadPool::submit(const std::function<void()> &task)
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        tasks_.push(task);
        cv_.notify_one(); // 通知一个线程处理任务
    }
    incrementTasksCount();
}

void ThreadPool::worker()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]
                     { return stop_ || !tasks_.empty(); }); // 等待新任务或停止信号

            if (stop_ && tasks_.empty())
            {
                return; // 退出线程
            }

            task = tasks_.front();
            tasks_.pop();
        }
        task(); // 执行任务
        decrementTasksCount();
    }
}

void ThreadPool::incrementTasksCount()
{
    std::lock_guard<std::mutex> lock(tasks_done_mutex_);
    active_tasks_count_++;
}

void ThreadPool::decrementTasksCount()
{
    std::lock_guard<std::mutex> lock(tasks_done_mutex_);
    active_tasks_count_--;
    if (active_tasks_count_ == 0)
    {
        tasks_done_cv_.notify_all();
    }
}

void ThreadPool::waitAllTasks()
{
    std::unique_lock<std::mutex> lock(tasks_done_mutex_);
    tasks_done_cv_.wait(lock, [this]
                        { return active_tasks_count_ == 0; });
}