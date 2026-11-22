#include "streamer/Future.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace streamer;
using namespace std::chrono;

TEST(Future, ValueSetBeforeAndAfterWaitingIsDelivered) {
    Promise<int> early;
    early.set_value(7);
    EXPECT_EQ(early.future().get(), 7);

    Promise<std::string> late;
    SharedFuture<std::string> f = late.future();
    EXPECT_TRUE(f.wait_for(milliseconds(0)) == std::future_status::timeout);
    std::thread t([&] { late.set_value("hello"); });
    EXPECT_EQ(f.get(), "hello");
    t.join();
    EXPECT_TRUE(f.wait_for(milliseconds(0)) == std::future_status::ready);
}

TEST(Future, ExceptionIsRethrownToEveryCopy) {
    Promise<int> p;
    SharedFuture<int> a = p.future(), b = a;
    p.set_exception(std::make_exception_ptr(std::runtime_error("boom")));
    EXPECT_THROW(a.get(), std::runtime_error);
    EXPECT_THROW(b.get(), std::runtime_error);
}

TEST(Future, VoidFuturesWork) {
    Promise<void> p;
    SharedFuture<void> f = p.future();
    std::thread t([&] { p.set_value(); });
    f.get();
    t.join();
    EXPECT_TRUE(f.valid());
}

TEST(Future, SettingTwiceThrowsAndKeepsTheFirstValue) {
    Promise<int> p;
    p.set_value(1);
    EXPECT_THROW(p.set_value(2), std::logic_error);
    EXPECT_THROW(p.set_exception(std::make_exception_ptr(std::runtime_error("late"))), std::logic_error);
    EXPECT_EQ(p.future().get(), 1);
}

TEST(Future, BrokenPromiseFailsWaitersInsteadOfHangingThem) {
    SharedFuture<int> f;
    {
        Promise<int> p;
        f = p.future();
    } // destroyed without a value
    EXPECT_THROW(f.get(), std::runtime_error);
}

TEST(Future, ManyWaitersAllSeeTheSameValue) {
    Promise<std::string> p;
    SharedFuture<std::string> f = p.future();
    std::atomic<int> matches{0};
    std::vector<std::thread> waiters;
    for (int i = 0; i < 8; ++i) waiters.emplace_back([&, f] { matches += f.get() == "ready"; });
    std::this_thread::sleep_for(milliseconds(5));
    p.set_value("ready");
    for (auto& t : waiters) t.join();
    EXPECT_EQ(matches.load(), 8);
}
