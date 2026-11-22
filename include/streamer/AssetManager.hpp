#pragma once

#include "streamer/AssetSource.hpp"
#include "streamer/LruCache.hpp"
#include "streamer/ThreadPool.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace streamer {

using AssetPtr = std::shared_ptr<const Asset>;
using AssetFuture = std::shared_future<AssetPtr>;

// Named priorities; any int works, higher runs first.
namespace priority {
constexpr int Low = 0;      // prefetch
constexpr int Normal = 1;
constexpr int High = 2;     // needed on screen now
constexpr int Critical = 3;
} // namespace priority

struct Options {
    std::size_t threads = 4;
    std::size_t cacheBytes = 64u * 1024u * 1024u;
};

// Delivered (through the future and the callback) to waiters of a load that cancel() removed.
class LoadCancelled : public std::runtime_error {
public:
    LoadCancelled() : std::runtime_error("asset load cancelled") {}
};

// Outcome of one request, delivered to callbacks by pumpCompletions().
struct Result {
    std::string path;
    AssetPtr asset;            // null on failure
    std::exception_ptr error;  // null on success
};
using Callback = std::function<void(const Result&)>;

struct Stats {
    std::uint64_t requests = 0;       // every request() call
    std::uint64_t cacheHits = 0;      // served from the LRU cache
    std::uint64_t coalesced = 0;      // joined a load that was already in flight
    std::uint64_t loadsStarted = 0;   // distinct loads queued (= cache misses)
    std::uint64_t loadsCompleted = 0;
    std::uint64_t loadsFailed = 0;
    std::uint64_t evictions = 0;
    std::uint64_t cancelled = 0;      // queued loads removed by cancel() before a worker started them
    std::uint64_t boosts = 0;         // times a later, higher-priority request promoted a queued load
    std::size_t cachedBytes = 0;
    std::size_t cachedAssets = 0;
    std::size_t cacheCapacity = 0;
    std::size_t workers = 0;          // size of the thread pool
    std::size_t activeLoads = 0;      // loads executing on a worker right now
    std::size_t queuedLoads = 0;      // loads waiting for a free worker
    // Request-to-ready latency (queue wait + read + decode) of the most recent loads, in ms.
    double meanLatencyMs = 0;
    double p95LatencyMs = 0;
    double maxLatencyMs = 0;
    double busyMs = 0;                // total worker time spent reading + decoding (utilisation = busyMs / (workers * wall))
};

// Asynchronous asset loader. request() never blocks on I/O: it returns a future immediately and a
// worker thread performs read + decode. Guarantees:
//   * the cache is an LRU with a byte budget, shared by all threads;
//   * concurrent requests for one path trigger exactly one load (request coalescing);
//   * failed loads are reported to every waiter and are not cached, so a later request retries;
//   * callbacks run only inside pumpCompletions(), i.e. on the thread that calls it (normally the
//     render thread), never on a worker.
// A request that joins an in-flight load with a higher priority promotes it (a second queue entry
// at the new priority; whichever entry a worker reaches first runs the load, the other is a no-op).
// request()/pumpCompletions()/stats() are safe to call from any thread.
class AssetManager {
public:
    AssetManager(std::shared_ptr<AssetSource> source, Options options);
    ~AssetManager(); // finishes queued loads, then joins the workers

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    AssetFuture request(const std::string& path, int priority = priority::Normal);
    AssetFuture request(const std::string& path, int priority, Callback onDone);

    // Cancels the load of `path` if it is still queued: every waiter's future throws LoadCancelled and
    // every callback receives it. Returns false (and changes nothing) if no such load is in flight or a
    // worker has already started it. It affects ALL waiters of that path, so only cancel what nobody
    // else needs. Exactly one of {completion, failure, cancellation} happens per load.
    bool cancel(const std::string& path);

    // Runs completed callbacks on the calling thread; returns how many ran.
    std::size_t pumpCompletions();

    Stats stats() const;

private:
    using Clock = std::chrono::steady_clock;
    struct LoadState {
        std::promise<AssetPtr> promise;
        std::atomic<bool> claimed{false}; // set by the first worker that starts this load
        Clock::time_point queuedAt = Clock::now();
    };
    struct Inflight {
        AssetFuture future;
        std::vector<Callback> waiters;
        std::shared_ptr<LoadState> state;
        int priority = 0;
    };

    AssetFuture requestImpl(const std::string& path, int priority, Callback onDone);
    void submitLoad(const std::string& path, const std::shared_ptr<LoadState>& state, int priority);
    void runLoad(const std::string& path, const std::shared_ptr<LoadState>& state);
    void pushCompletion(Callback cb, Result result);

    std::shared_ptr<AssetSource> source_;
    LruCache<Asset> cache_;

    mutable std::mutex mutex_; // guards inflight_; lock order: mutex_ -> cache_'s internal mutex
    std::unordered_map<std::string, Inflight> inflight_;

    std::mutex doneMutex_;
    std::deque<std::pair<Callback, Result>> done_;

    std::atomic<std::uint64_t> requests_{0}, hits_{0}, coalesced_{0}, started_{0}, completed_{0}, failed_{0}, cancelled_{0}, boosts_{0}, busyNanos_{0};
    std::atomic<std::size_t> active_{0};

    mutable std::mutex latencyMutex_;
    std::vector<double> latenciesMs_; // ring of the most recent load latencies
    std::size_t latencyNext_ = 0;

    ThreadPool pool_; // declared last: destroyed first, so workers stop before the state above goes away
};

} // namespace streamer
