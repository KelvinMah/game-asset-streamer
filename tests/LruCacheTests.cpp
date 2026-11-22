#include "streamer/LruCache.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace streamer;

namespace {
std::shared_ptr<const int> val(int v) { return std::make_shared<const int>(v); }
}

TEST(LruCache, EvictsLeastRecentlyUsedFirst) {
    LruCache<int> cache(3);
    cache.put("a", val(1), 1);
    cache.put("b", val(2), 1);
    cache.put("c", val(3), 1);
    ASSERT_TRUE(cache.get("a") != nullptr); // a is now most recent; b is the oldest
    cache.put("d", val(4), 1);
    EXPECT_TRUE(cache.get("b") == nullptr);
    EXPECT_TRUE(cache.get("a") != nullptr);
    EXPECT_TRUE(cache.get("c") != nullptr);
    EXPECT_TRUE(cache.get("d") != nullptr);
    EXPECT_EQ(cache.evictions(), 1U);
}

TEST(LruCache, ByteBudgetCountsSizesNotEntries) {
    LruCache<int> cache(10);
    cache.put("a", val(1), 4);
    cache.put("b", val(2), 4);
    cache.put("c", val(3), 6); // 14 > 10: evicts a (oldest), leaving b + c = 10
    EXPECT_TRUE(cache.get("a") == nullptr);
    EXPECT_TRUE(cache.get("b") != nullptr);
    EXPECT_TRUE(cache.get("c") != nullptr);
    EXPECT_EQ(cache.bytes(), 10U);
    EXPECT_EQ(cache.size(), 2U);
}

TEST(LruCache, OversizedValueIsNotCached) {
    LruCache<int> cache(8);
    cache.put("a", val(1), 4);
    EXPECT_FALSE(cache.put("huge", val(2), 9));
    EXPECT_TRUE(cache.get("huge") == nullptr);
    EXPECT_TRUE(cache.get("a") != nullptr); // existing entries untouched
    EXPECT_EQ(cache.bytes(), 4U);
}

TEST(LruCache, ReplacingAKeyUpdatesItsCost) {
    LruCache<int> cache(10);
    cache.put("a", val(1), 8);
    cache.put("a", val(2), 3);
    EXPECT_EQ(cache.bytes(), 3U);
    EXPECT_EQ(cache.size(), 1U);
    EXPECT_EQ(*cache.get("a"), 2);
}

TEST(LruCache, ZeroCapacityCachesNothing) {
    LruCache<int> cache(0);
    EXPECT_FALSE(cache.put("a", val(1), 1));
    EXPECT_TRUE(cache.get("a") == nullptr);
}

TEST(LruCache, EvictedValueStaysAliveWhileHeldAndIsFreedAfter) {
    LruCache<int> cache(1);
    auto held = val(42);
    std::weak_ptr<const int> watcher = held;
    cache.put("a", held, 1);
    cache.put("b", val(1), 1); // evicts "a"
    EXPECT_TRUE(cache.get("a") == nullptr);
    ASSERT_FALSE(watcher.expired());
    EXPECT_EQ(*held, 42); // still valid for its holder
    held.reset();
    EXPECT_TRUE(watcher.expired()); // no leak: the cache no longer references it
}

TEST(LruCache, ConcurrentUseKeepsTheBudgetInvariant) {
    LruCache<int> cache(50);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 2000; ++i) {
                const std::string key = "k" + std::to_string((i * 7 + t) % 100);
                if (!cache.get(key)) cache.put(key, val(i), 1 + (i % 3));
                EXPECT_LE(cache.bytes(), cache.capacity());
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_LE(cache.bytes(), 50U);
}
