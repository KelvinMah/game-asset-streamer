#include "streamer/AssetManager.hpp"
#include "streamer/Png.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace streamer;
using namespace std::chrono;

namespace {

constexpr std::size_t AssetBytes = 4096;

// SyntheticSource plus read counting, optional delay, and "missing*" paths that fail.
class CountingSource : public AssetSource {
public:
    explicit CountingSource(microseconds delay = microseconds(0)) : delay_(delay) {}
    Bytes read(const std::string& path) override {
        ++reads;
        if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
        if (path.rfind("missing", 0) == 0) throw std::runtime_error("no such asset: " + path);
        return SyntheticSource::generate(path, AssetBytes);
    }
    std::atomic<int> reads{0};

private:
    microseconds delay_;
};

std::string name(int i) { return "asset_" + std::to_string(i) + ".tex"; }

void expectIntact(const AssetPtr& asset, const std::string& path) {
    ASSERT_TRUE(asset != nullptr);
    EXPECT_EQ(asset->path, path);
    EXPECT_EQ(asset->bytes.size(), AssetBytes);
    EXPECT_EQ(asset->checksum, checksum(SyntheticSource::generate(path, AssetBytes)));
}

} // namespace

TEST(AssetManager, LoadsAndClassifiesAssets) {
    AssetManager m(std::make_shared<CountingSource>(), {2, 1 << 20});
    const AssetPtr tex = m.request("hero.tex").get();
    const AssetPtr snd = m.request("jump.wav").get();
    const AssetPtr dat = m.request("level.json").get();
    EXPECT_TRUE(tex->type == AssetType::Texture);
    EXPECT_TRUE(snd->type == AssetType::Audio);
    EXPECT_TRUE(dat->type == AssetType::Data);
    EXPECT_EQ(tex->checksum, checksum(tex->bytes));
}

TEST(AssetManager, RequestReturnsImmediatelyWhileTheLoadRunsInTheBackground) {
    AssetManager m(std::make_shared<CountingSource>(milliseconds(200)), {2, 1 << 20});
    const auto start = steady_clock::now();
    AssetFuture f = m.request(name(1));
    const auto callTime = steady_clock::now() - start;
    EXPECT_LT(duration_cast<milliseconds>(callTime).count(), 100); // the 200 ms read did not block us
    EXPECT_TRUE(f.wait_for(milliseconds(0)) == std::future_status::timeout);
    expectIntact(f.get(), name(1));
}

TEST(AssetManager, SecondRequestIsServedFromCache) {
    auto source = std::make_shared<CountingSource>();
    AssetManager m(source, {2, 1 << 20});
    const AssetPtr first = m.request(name(1)).get();
    const AssetPtr second = m.request(name(1)).get();
    EXPECT_EQ(source->reads.load(), 1);
    EXPECT_TRUE(first.get() == second.get()); // same shared object
    EXPECT_EQ(m.stats().cacheHits, 1U);
}

TEST(AssetManager, ConcurrentRequestsForOnePathLoadItExactlyOnce) {
    auto source = std::make_shared<CountingSource>(milliseconds(50));
    AssetManager m(source, {4, 1 << 20});
    std::atomic<bool> go{false};
    std::vector<AssetPtr> got(64);
    std::vector<std::thread> threads;
    for (int t = 0; t < 64; ++t) {
        threads.emplace_back([&, t] {
            while (!go) std::this_thread::yield();
            got[t] = m.request("shared.tex").get();
        });
    }
    go = true;
    for (auto& th : threads) th.join();

    EXPECT_EQ(source->reads.load(), 1);
    for (const auto& g : got) EXPECT_TRUE(g.get() == got[0].get());
    const Stats s = m.stats();
    EXPECT_EQ(s.loadsStarted, 1U);
    EXPECT_EQ(s.coalesced + s.cacheHits, 63U);
}

TEST(AssetManager, FailedLoadsReachEveryWaiterAndAreNotCached) {
    auto source = std::make_shared<CountingSource>(milliseconds(20));
    AssetManager m(source, {2, 1 << 20});
    AssetFuture a = m.request("missing_a.tex");
    AssetFuture b = m.request("missing_a.tex"); // joins the failing load
    EXPECT_THROW(a.get(), std::runtime_error);
    EXPECT_THROW(b.get(), std::runtime_error);
    EXPECT_THROW(m.request("missing_a.tex").get(), std::runtime_error); // retried, not a cached failure
    EXPECT_EQ(source->reads.load(), 2);
    EXPECT_EQ(m.stats().loadsFailed, 2U);
    expectIntact(m.request(name(1)).get(), name(1)); // the manager is still healthy
}

TEST(AssetManager, CallbacksRunOnThePumpingThreadNotOnWorkers) {
    AssetManager m(std::make_shared<CountingSource>(), {2, 1 << 20});
    std::vector<std::thread::id> callbackThreads;
    std::vector<std::string> done;
    auto cb = [&](const Result& r) {
        callbackThreads.push_back(std::this_thread::get_id());
        done.push_back(r.path);
        EXPECT_TRUE(r.asset != nullptr);
    };
    m.request(name(1), priority::Normal, cb).get();
    EXPECT_TRUE(done.empty()); // loaded, but nothing runs until we pump
    EXPECT_EQ(m.pumpCompletions(), 1U);
    m.request(name(1), priority::Normal, cb); // cache hit still delivers through the pump
    EXPECT_EQ(m.pumpCompletions(), 1U);
    ASSERT_EQ(done.size(), 2U);
    for (const auto& id : callbackThreads) EXPECT_TRUE(id == std::this_thread::get_id());
}

TEST(AssetManager, CallbackReceivesTheErrorForFailedLoads) {
    AssetManager m(std::make_shared<CountingSource>(), {1, 1 << 20});
    bool failed = false;
    AssetFuture f = m.request("missing_b.tex", priority::Normal, [&](const Result& r) { failed = r.error != nullptr && !r.asset; });
    EXPECT_THROW(f.get(), std::runtime_error);
    m.pumpCompletions();
    EXPECT_TRUE(failed);
}

TEST(AssetManager, HighPriorityRequestsOvertakeQueuedLowPriorityOnes) {
    // One worker, slow reads: flood with low-priority loads, then ask for one high-priority asset.
    AssetManager m(std::make_shared<CountingSource>(milliseconds(10)), {1, 0});
    std::vector<AssetFuture> low;
    for (int i = 0; i < 20; ++i) low.push_back(m.request(name(i), priority::Low));
    AssetFuture high = m.request("urgent.tex", priority::High);
    high.get();
    int lowStillPending = 0;
    for (auto& f : low) lowStillPending += f.wait_for(milliseconds(0)) == std::future_status::timeout;
    EXPECT_GE(lowStillPending, 15); // urgent finished while nearly all of the earlier low-priority work waited
}

TEST(AssetManager, TenThousandConcurrentRequestsStayCorrectUnderEviction) {
    constexpr int Threads = 8, PerThread = 1250, Distinct = 200;
    auto source = std::make_shared<CountingSource>();
    // Budget holds ~40 of the 200 assets, so the run is dominated by eviction and reloads.
    AssetManager m(source, {4, 40 * AssetBytes});

    std::vector<std::uint64_t> expected(Distinct);
    for (int i = 0; i < Distinct; ++i) expected[i] = checksum(SyntheticSource::generate(name(i), AssetBytes));

    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t) {
        threads.emplace_back([&, t] {
            std::uint32_t rng = 2463534242u + static_cast<std::uint32_t>(t);
            std::vector<std::pair<int, AssetFuture>> batch;
            for (int i = 0; i < PerThread; ++i) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                const int id = static_cast<int>(rng % Distinct);
                batch.emplace_back(id, m.request(name(id), static_cast<int>(rng % 3)));
                if (batch.size() == 50 || i == PerThread - 1) {
                    for (auto& [want, fut] : batch) {
                        const AssetPtr a = fut.get();
                        if (!a || a->path != name(want) || a->checksum != expected[want] || checksum(a->bytes) != expected[want]) ++bad;
                    }
                    batch.clear();
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(bad.load(), 0);
    const Stats s = m.stats();
    EXPECT_EQ(s.requests, static_cast<std::uint64_t>(Threads * PerThread));
    EXPECT_EQ(s.cacheHits + s.coalesced + s.loadsStarted, s.requests); // every request accounted for once
    EXPECT_EQ(s.loadsCompleted + s.loadsFailed, s.loadsStarted);
    EXPECT_EQ(s.loadsFailed, 0U);
    EXPECT_EQ(static_cast<std::uint64_t>(source->reads.load()), s.loadsStarted);
    EXPECT_GT(s.evictions, 0U);
    EXPECT_LE(s.cachedBytes, 40U * AssetBytes);
}

TEST(AssetManager, DestroyingTheManagerReleasesEveryAsset) {
    std::weak_ptr<const Asset> watcher;
    {
        AssetManager m(std::make_shared<CountingSource>(), {2, 1 << 20});
        watcher = m.request(name(1)).get();
        for (int i = 2; i < 30; ++i) m.request(name(i)); // queued loads finish during destruction
        EXPECT_FALSE(watcher.expired());                  // held by the cache
    }
    EXPECT_TRUE(watcher.expired()); // nothing leaked past the manager's lifetime
}

TEST(FileSource, ReadsFilesAndRejectsPathEscapes) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "streamer_test_assets";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "a.bin", std::ios::binary);
        f << "hello";
    }
    FileSource source(dir.string());
    EXPECT_EQ(source.read("a.bin").size(), 5U);
    EXPECT_THROW(source.read("nope.bin"), std::runtime_error);
    EXPECT_THROW(source.read("../secret"), std::invalid_argument);
    EXPECT_THROW(source.read("/etc/passwd"), std::invalid_argument);
    EXPECT_THROW(source.read("C:\\Windows\\win.ini"), std::exception);       // drive path: rejected or unreadable everywhere
    fs::remove_all(dir);
}

// ---- priority promotion and live metrics -----------------------------------------------------

TEST(AssetManager, LaterHighPriorityRequestPromotesAQueuedLowPriorityLoad) {
    // One worker. "target" is queued LAST at Low priority; asking for it again at High must move it
    // ahead of everything else still waiting.
    AssetManager m(std::make_shared<CountingSource>(milliseconds(10)), {1, 0});
    std::vector<AssetFuture> filler;
    for (int i = 0; i < 20; ++i) filler.push_back(m.request(name(i), priority::Low));
    AssetFuture lowTarget = m.request("target.tex", priority::Low);
    AssetFuture promoted = m.request("target.tex", priority::High); // same in-flight load
    promoted.get();
    EXPECT_TRUE(lowTarget.wait_for(milliseconds(0)) == std::future_status::ready); // it is the same load
    int fillerPending = 0;
    for (auto& f : filler) fillerPending += f.wait_for(milliseconds(0)) == std::future_status::timeout;
    EXPECT_GE(fillerPending, 15); // target finished while nearly all earlier-queued filler still waited
    const Stats s = m.stats();
    EXPECT_EQ(s.boosts, 1U);
    EXPECT_EQ(s.loadsStarted, 21U); // promotion never starts a second load for the same path
    EXPECT_EQ(m.request("target.tex").get()->path, "target.tex");
}

TEST(AssetManager, EqualOrLowerPriorityRequestsDoNotCountAsBoosts) {
    AssetManager m(std::make_shared<CountingSource>(milliseconds(20)), {1, 0});
    AssetFuture a = m.request("x.tex", priority::High);
    m.request("x.tex", priority::High);
    m.request("x.tex", priority::Low);
    a.get();
    EXPECT_EQ(m.stats().boosts, 0U);
}

TEST(AssetManager, PromotedLoadRunsExactlyOnce) {
    auto source = std::make_shared<CountingSource>(milliseconds(2));
    AssetManager m(source, {4, 0});
    for (int round = 0; round < 50; ++round) {
        const std::string p = "twice_" + std::to_string(round) + ".tex";
        AssetFuture low = m.request(p, priority::Low);
        AssetFuture high = m.request(p, priority::Critical);
        low.get();
        high.get();
    }
    EXPECT_EQ(source->reads.load(), 50); // each path read once even though it was queued twice
}

TEST(AssetManager, StatsReportWorkersQueueDepthAndLatency) {
    AssetManager m(std::make_shared<CountingSource>(milliseconds(40)), {2, 0});
    std::vector<AssetFuture> fs;
    for (int i = 0; i < 6; ++i) fs.push_back(m.request(name(i), priority::Normal));
    // Wait until both workers have picked up a load.
    for (int spin = 0; spin < 2000 && m.stats().activeLoads < 2; ++spin) std::this_thread::sleep_for(milliseconds(1));
    Stats mid = m.stats();
    EXPECT_EQ(mid.workers, 2U);
    EXPECT_EQ(mid.activeLoads, 2U);
    EXPECT_EQ(mid.queuedLoads, 4U);
    for (auto& f : fs) f.get();
    const Stats end = m.stats();
    EXPECT_EQ(end.activeLoads, 0U);
    EXPECT_EQ(end.queuedLoads, 0U);
    EXPECT_GT(end.meanLatencyMs, 30.0);               // each load slept 40 ms
    EXPECT_GE(end.p95LatencyMs, end.meanLatencyMs - 1);
    EXPECT_GE(end.maxLatencyMs, end.p95LatencyMs);
}

// ---- real PNG files through the whole pipeline -------------------------------------------------

namespace {
struct PngAssetDir {
    std::filesystem::path dir;
    std::vector<std::uint64_t> pixelChecksums;
    std::vector<int> widths;

    explicit PngAssetDir(int count) {
        dir = std::filesystem::temp_directory_path() / "streamer_png_assets";
        std::filesystem::create_directories(dir);
        for (int i = 0; i < count; ++i) {
            Image img;
            img.width = 16 + (i % 5) * 8;
            img.height = 16 + (i % 3) * 8;
            img.rgba.resize(static_cast<std::size_t>(img.width) * img.height * 4);
            for (std::size_t k = 0; k < img.rgba.size(); ++k) img.rgba[k] = static_cast<std::uint8_t>((k * (i + 3) + i) & 255);
            const Bytes png = encodePng(img);
            std::ofstream f(dir / ("img_" + std::to_string(i) + ".png"), std::ios::binary);
            f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
            pixelChecksums.push_back(checksum(img.rgba));
            widths.push_back(img.width);
        }
    }
    ~PngAssetDir() { std::filesystem::remove_all(dir); }
};
} // namespace

TEST(AssetManager, LoadsAndDecodesRealPngFiles) {
    PngAssetDir assets(4);
    AssetManager m(std::make_shared<FileSource>(assets.dir.string()), {2, 1 << 20});
    for (int i = 0; i < 4; ++i) {
        const AssetPtr a = m.request("img_" + std::to_string(i) + ".png").get();
        EXPECT_TRUE(a->type == AssetType::Texture);
        EXPECT_EQ(a->width, assets.widths[i]);
        EXPECT_EQ(a->bytes.size(), static_cast<std::size_t>(a->width) * a->height * 4); // decoded RGBA, not file bytes
        EXPECT_EQ(a->checksum, assets.pixelChecksums[i]);
    }
}

TEST(AssetManager, CorruptPngFailsTheLoadWithoutPoisoningTheCache) {
    PngAssetDir assets(1);
    {
        std::ofstream f(assets.dir / "broken.png", std::ios::binary);
        f << "this is not a png";
    }
    AssetManager m(std::make_shared<FileSource>(assets.dir.string()), {2, 1 << 20});
    EXPECT_THROW(m.request("broken.png").get(), std::runtime_error);
    EXPECT_THROW(m.request("broken.png").get(), std::runtime_error); // retried, not cached
    EXPECT_EQ(m.request("img_0.png").get()->width, assets.widths[0]);
    EXPECT_EQ(m.stats().loadsFailed, 2U);
}

TEST(AssetManager, StressWithRealPngFilesUnderConstantEviction) {
    constexpr int Files = 60, Threads = 8, PerThread = 1250;
    PngAssetDir assets(Files);
    // Budget for roughly a quarter of the decoded images, so most requests evict something.
    AssetManager m(std::make_shared<FileSource>(assets.dir.string()), {4, 15 * 40 * 40 * 4});
    std::atomic<int> bad{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t) {
        threads.emplace_back([&, t] {
            std::uint32_t rng = 88172645u + static_cast<std::uint32_t>(t) * 977u;
            for (int i = 0; i < PerThread; ++i) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                const int id = static_cast<int>(rng % Files);
                const AssetPtr a = m.request("img_" + std::to_string(id) + ".png", static_cast<int>(rng % 3)).get();
                if (!a || a->checksum != assets.pixelChecksums[id] || checksum(a->bytes) != assets.pixelChecksums[id]) ++bad;
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(bad.load(), 0);
    const Stats s = m.stats();
    EXPECT_EQ(s.requests, static_cast<std::uint64_t>(Threads * PerThread));
    EXPECT_EQ(s.cacheHits + s.coalesced + s.loadsStarted, s.requests);
    EXPECT_EQ(s.loadsFailed, 0U);
    EXPECT_GT(s.evictions, 0U);
    EXPECT_LE(s.cachedBytes, s.cacheCapacity);
}

// ---- cancellation ------------------------------------------------------------------------------

namespace {
// Waits until `n` loads are executing on workers, so the rest are known to be queued.
void waitForActive(AssetManager& m, std::size_t n) {
    for (int spin = 0; spin < 5000 && m.stats().activeLoads < n; ++spin) std::this_thread::sleep_for(milliseconds(1));
    ASSERT_EQ(m.stats().activeLoads, n);
}

bool isCancelled(const std::exception_ptr& e) {
    if (!e) return false;
    try {
        std::rethrow_exception(e);
    } catch (const LoadCancelled&) {
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace

TEST(Cancel, QueuedLoadIsCancelledAndNeverRead) {
    auto source = std::make_shared<CountingSource>(milliseconds(60));
    AssetManager m(source, {1, 1 << 20});
    AssetFuture running = m.request("running.tex");
    waitForActive(m, 1);
    int callbacks = 0;
    bool sawCancel = false;
    AssetFuture queued = m.request("queued.tex", priority::Normal, [&](const Result& r) { ++callbacks; sawCancel = isCancelled(r.error) && !r.asset; });

    EXPECT_TRUE(m.cancel("queued.tex"));
    EXPECT_THROW(queued.get(), LoadCancelled);
    EXPECT_EQ(m.pumpCompletions(), 1U);
    EXPECT_EQ(callbacks, 1);
    EXPECT_TRUE(sawCancel);
    expectIntact(running.get(), "running.tex");
    EXPECT_EQ(source->reads.load(), 1); // "queued.tex" was never read
    const Stats s = m.stats();
    EXPECT_EQ(s.cancelled, 1U);
    EXPECT_EQ(s.loadsStarted, s.loadsCompleted + s.loadsFailed + s.cancelled);
    EXPECT_EQ(s.queuedLoads, 0U);
}

TEST(Cancel, ALoadAlreadyRunningCannotBeCancelled) {
    AssetManager m(std::make_shared<CountingSource>(milliseconds(60)), {1, 1 << 20});
    AssetFuture f = m.request("running.tex");
    waitForActive(m, 1);
    EXPECT_FALSE(m.cancel("running.tex"));
    expectIntact(f.get(), "running.tex"); // it completes normally
    EXPECT_EQ(m.stats().cancelled, 0U);
}

TEST(Cancel, UnknownOrFinishedPathsReturnFalse) {
    AssetManager m(std::make_shared<CountingSource>(), {1, 1 << 20});
    EXPECT_FALSE(m.cancel("never_requested.tex"));
    m.request("done.tex").get();
    EXPECT_FALSE(m.cancel("done.tex"));
}

TEST(Cancel, EveryWaiterIsNotifiedExactlyOnce) {
    AssetManager m(std::make_shared<CountingSource>(milliseconds(60)), {1, 1 << 20});
    m.request("running.tex");
    waitForActive(m, 1);
    std::atomic<int> cancelledCallbacks{0}, otherCallbacks{0};
    auto cb = [&](const Result& r) { (isCancelled(r.error) ? cancelledCallbacks : otherCallbacks)++; };
    AssetFuture a = m.request("q.tex", priority::Low, cb);
    AssetFuture b = m.request("q.tex", priority::Low, cb); // coalesced onto the same queued load
    AssetFuture c = m.request("q.tex", priority::Low, cb);
    EXPECT_TRUE(m.cancel("q.tex"));
    EXPECT_FALSE(m.cancel("q.tex")); // second cancel finds nothing
    for (AssetFuture* f : {&a, &b, &c}) EXPECT_THROW(f->get(), LoadCancelled);
    m.pumpCompletions();
    EXPECT_EQ(cancelledCallbacks.load(), 3);
    EXPECT_EQ(otherCallbacks.load(), 0);
}

TEST(Cancel, ARequestAfterCancellationStartsAFreshLoad) {
    auto source = std::make_shared<CountingSource>(milliseconds(40));
    AssetManager m(source, {1, 1 << 20});
    m.request("running.tex");
    waitForActive(m, 1);
    AssetFuture first = m.request("again.tex", priority::Low);
    ASSERT_TRUE(m.cancel("again.tex"));
    EXPECT_THROW(first.get(), LoadCancelled);
    AssetFuture second = m.request("again.tex", priority::High);
    expectIntact(second.get(), "again.tex");
    EXPECT_EQ(m.stats().loadsStarted, 3U); // running + cancelled + fresh
}

TEST(Cancel, RaceBetweenCancelAndWorkerClaimResolvesExactlyOnce) {
    // Each load is either cancelled or completed, never both and never twice (a double
    // set_value/set_exception on the promise would throw std::logic_error and abort the run).
    constexpr int Rounds = 1500;
    auto source = std::make_shared<CountingSource>();
    AssetManager m(source, {2, 1 << 22});
    int cancelledCount = 0, completedCount = 0;
    for (int i = 0; i < Rounds; ++i) {
        const std::string p = "race_" + std::to_string(i) + ".tex";
        AssetFuture f = m.request(p, priority::Normal);
        bool won = false;
        std::thread canceller([&] { won = m.cancel(p); });
        canceller.join();
        try {
            expectIntact(f.get(), p);
            ++completedCount;
            EXPECT_FALSE(won);
        } catch (const LoadCancelled&) {
            ++cancelledCount;
            EXPECT_TRUE(won);
        }
    }
    EXPECT_EQ(cancelledCount + completedCount, Rounds);
    EXPECT_EQ(static_cast<std::uint64_t>(completedCount), m.stats().loadsCompleted);
    EXPECT_EQ(static_cast<std::uint64_t>(cancelledCount), m.stats().cancelled);
    EXPECT_EQ(source->reads.load(), completedCount); // cancelled loads were never read
}

TEST(Cancel, ConcurrentRequestsAndCancelsDeliverEveryCallbackExactlyOnce) {
    constexpr int Threads = 8, OpsPerThread = 2500, Paths = 50;
    auto source = std::make_shared<CountingSource>(microseconds(100));
    AssetManager m(source, {3, 8 * AssetBytes});
    std::atomic<int> requestsWithCallback{0}, callbacks{0}, corrupt{0};
    std::mutex pumpMutex;

    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t) {
        threads.emplace_back([&, t] {
            std::uint32_t rng = 7919u * static_cast<std::uint32_t>(t + 1);
            for (int i = 0; i < OpsPerThread; ++i) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                const std::string p = name(static_cast<int>(rng % Paths));
                switch ((rng >> 8) % 4) {
                case 0: m.cancel(p); break;
                case 1: {
                    ++requestsWithCallback;
                    m.request(p, static_cast<int>((rng >> 12) % 4), [&, p](const Result& r) {
                        ++callbacks;
                        if (r.asset && r.asset->checksum != checksum(SyntheticSource::generate(p, AssetBytes))) ++corrupt;
                    });
                    break;
                }
                case 2: { // a future-only request; wait on it half the time
                    AssetFuture f = m.request(p, priority::Low);
                    if (rng & 0x10000) {
                        try { f.get(); } catch (const LoadCancelled&) {}
                    }
                    break;
                }
                default: {
                    std::lock_guard<std::mutex> lock(pumpMutex);
                    m.pumpCompletions();
                }
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    // Drain: every started load must finish one way or another.
    for (int spin = 0; spin < 10000; ++spin) {
        const Stats s = m.stats();
        if (s.loadsStarted == s.loadsCompleted + s.loadsFailed + s.cancelled) break;
        std::this_thread::sleep_for(milliseconds(1));
    }
    m.pumpCompletions();

    const Stats s = m.stats();
    EXPECT_EQ(s.loadsStarted, s.loadsCompleted + s.loadsFailed + s.cancelled);
    EXPECT_EQ(s.loadsFailed, 0U);
    EXPECT_EQ(callbacks.load(), requestsWithCallback.load()); // none lost, none doubled
    EXPECT_EQ(corrupt.load(), 0);
    EXPECT_EQ(static_cast<std::uint64_t>(source->reads.load()), s.loadsCompleted);
    EXPECT_GT(s.cancelled, 0U);   // the test actually exercised cancellation
    EXPECT_GT(s.loadsCompleted, 0U);
    EXPECT_EQ(s.activeLoads, 0U);
    EXPECT_EQ(s.queuedLoads, 0U);
}

TEST(Cancel, ShutdownWithPendingAndCancelledJobsCompletesEveryFuture) {
    std::vector<AssetFuture> futures;
    std::atomic<int> reads{0};
    const auto start = steady_clock::now();
    {
        auto source = std::make_shared<CountingSource>(milliseconds(3));
        AssetManager m(source, {2, 1 << 20});
        for (int i = 0; i < 100; ++i) futures.push_back(m.request(name(i), i % 4, [](const Result&) {}));
        for (int i = 0; i < 100; i += 3) m.cancel(name(i)); // some win, some lose against the workers
        reads = 0; // (only used to keep the scope obvious)
    } // destructor must drain queued work and join without deadlocking
    EXPECT_LT(duration_cast<seconds>(steady_clock::now() - start).count(), 10);
    int done = 0, cancelled = 0;
    for (auto& f : futures) {
        ASSERT_TRUE(f.wait_for(milliseconds(0)) == std::future_status::ready); // nothing left dangling
        try { f.get(); ++done; } catch (const LoadCancelled&) { ++cancelled; }
    }
    EXPECT_EQ(done + cancelled, 100);
    EXPECT_GT(cancelled, 0);
}
