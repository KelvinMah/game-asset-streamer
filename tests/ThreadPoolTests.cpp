#include "streamer/ThreadPool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace streamer;

namespace {
// Holds the pool's only worker busy so later submissions queue up and ordering is observable.
struct Gate {
    Promise<void> release;
    std::atomic<bool> started{false};
    SharedFuture<void> wait = release.future();
};
} // namespace

TEST(ThreadPool, RunsTasksAndReturnsValues) {
    ThreadPool pool(3);
    std::vector<SharedFuture<int>> results;
    for (int i = 0; i < 50; ++i) results.push_back(pool.submit(0, [i] { return i * i; }));
    for (int i = 0; i < 50; ++i) EXPECT_EQ(results[i].get(), i * i);
}

TEST(ThreadPool, HigherPriorityRunsFirstAndTiesAreFifo) {
    ThreadPool pool(1);
    Gate gate;
    auto blocker = pool.submit(0, [&] {
        gate.started = true;
        gate.wait.wait();
    });
    while (!gate.started) std::this_thread::yield();

    std::mutex m;
    std::vector<int> order;
    auto record = [&](int id) { return [&, id] { std::lock_guard<std::mutex> l(m); order.push_back(id); }; };
    pool.submit(0, record(1));   // low
    pool.submit(1, record(2));   // normal
    pool.submit(2, record(3));   // high, first
    pool.submit(3, record(4));   // critical
    pool.submit(2, record(5));   // high, second
    pool.submit(0, record(6));   // low, second
    gate.release.set_value();
    pool.shutdown(); // drains

    EXPECT_EQ(order, (std::vector<int>{4, 3, 5, 2, 1, 6}));
}

TEST(ThreadPool, ExceptionsTravelThroughTheFuture) {
    ThreadPool pool(2);
    auto f = pool.submit(0, []() -> int { throw std::runtime_error("boom"); });
    EXPECT_THROW(f.get(), std::runtime_error);
    EXPECT_EQ(pool.submit(0, [] { return 7; }).get(), 7); // the worker survived
}

TEST(ThreadPool, ShutdownDrainsQueuedWorkAndRejectsNewWork) {
    std::atomic<int> ran{0};
    ThreadPool pool(2);
    for (int i = 0; i < 200; ++i) pool.submit(0, [&] { ++ran; });
    pool.shutdown();
    EXPECT_EQ(ran.load(), 200);
    EXPECT_THROW(pool.submit(0, [] {}), std::runtime_error);
    pool.shutdown(); // idempotent
}

TEST(ThreadPool, ZeroThreadsIsRejected) { EXPECT_THROW(ThreadPool(0), std::invalid_argument); }

TEST(ThreadPool, ManyProducersAllTasksRunExactlyOnce) {
    std::atomic<int> ran{0};
    {
        ThreadPool pool(4);
        std::vector<std::thread> producers;
        for (int t = 0; t < 8; ++t) {
            producers.emplace_back([&, t] {
                for (int i = 0; i < 500; ++i) pool.submit((t + i) % 4, [&] { ++ran; });
            });
        }
        for (auto& p : producers) p.join();
    } // destructor drains
    EXPECT_EQ(ran.load(), 4000);
}
