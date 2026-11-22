#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace streamer {

// Fixed-size worker pool with a priority queue. Higher `priority` runs first; tasks with equal
// priority run in submission order (FIFO), so a stream of equal-priority work cannot starve itself.
// Shutdown drains: every task submitted before shutdown() still runs.
class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads) {
        if (threads == 0) throw std::invalid_argument("ThreadPool needs at least one thread");
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Queues `fn`. Exceptions thrown by `fn` are delivered through the returned future.
    // Throws std::runtime_error if the pool has been shut down.
    template <class F>
    auto submit(int priority, F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
        using R = std::invoke_result_t<std::decay_t<F>>;
        auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(fn));
        std::future<R> result = task->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("ThreadPool is shut down");
            queue_.push(Task{priority, nextSequence_++, [task] { (*task)(); }});
        }
        wake_.notify_one();
        return result;
    }

    // Stops accepting work, runs everything already queued, and joins the workers. Idempotent.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        std::lock_guard<std::mutex> joinLock(joinMutex_);
        for (std::thread& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    std::size_t pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t threadCount() const { return workers_.size(); }

private:
    struct Task {
        int priority = 0;
        std::uint64_t sequence = 0;
        std::function<void()> run;
    };

    struct Lower { // "less urgent" ordering for std::priority_queue (top = most urgent)
        bool operator()(const Task& a, const Task& b) const {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.sequence > b.sequence;
        }
    };

    void workerLoop() {
        for (;;) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) return; // stopping and fully drained
                task = queue_.top();
                queue_.pop();
            }
            task.run();
        }
    }

    mutable std::mutex mutex_;
    std::mutex joinMutex_;
    std::condition_variable wake_;
    std::priority_queue<Task, std::vector<Task>, Lower> queue_;
    std::uint64_t nextSequence_ = 0;
    bool stopping_ = false;
    std::vector<std::thread> workers_;
};

} // namespace streamer
