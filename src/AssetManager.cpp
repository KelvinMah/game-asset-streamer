#include "streamer/AssetManager.hpp"

#include <algorithm>

namespace streamer {

AssetManager::AssetManager(std::shared_ptr<AssetSource> source, Options options)
    : source_(std::move(source)), cache_(options.cacheBytes), pool_(options.threads) {
    if (!source_) throw std::invalid_argument("AssetManager needs an AssetSource");
}

AssetManager::~AssetManager() { pool_.shutdown(); }

AssetFuture AssetManager::request(const std::string& path, int priority) {
    return requestImpl(path, priority, nullptr);
}

AssetFuture AssetManager::request(const std::string& path, int priority, Callback onDone) {
    return requestImpl(path, priority, std::move(onDone));
}

AssetFuture AssetManager::requestImpl(const std::string& path, int priority, Callback onDone) {
    ++requests_;
    std::unique_lock<std::mutex> lock(mutex_);

    // 1. Already loading? Join it. Checked first: a finished load stores into the cache *before*
    //    it leaves inflight_, so "not in flight" below means "in the cache, or never loaded".
    const auto running = inflight_.find(path);
    if (running != inflight_.end()) {
        ++coalesced_;
        Inflight& entry = running->second;
        if (onDone) entry.waiters.push_back(std::move(onDone));
        if (priority > entry.priority) { // promote: queue a second entry at the higher priority
            entry.priority = priority;
            ++boosts_;
            submitLoad(path, entry.state, priority);
        }
        return entry.future;
    }

    // 2. Cached?
    if (AssetPtr cached = cache_.get(path)) {
        lock.unlock();
        ++hits_;
        std::promise<AssetPtr> ready;
        ready.set_value(cached);
        if (onDone) pushCompletion(std::move(onDone), Result{path, cached, nullptr});
        return ready.get_future().share();
    }

    // 3. Start a load.
    auto state = std::make_shared<LoadState>();
    AssetFuture future = state->promise.get_future().share();
    Inflight& entry = inflight_[path];
    entry.future = future;
    entry.state = state;
    entry.priority = priority;
    if (onDone) entry.waiters.push_back(std::move(onDone));
    lock.unlock();

    ++started_;
    submitLoad(path, state, priority);
    return future;
}

void AssetManager::submitLoad(const std::string& path, const std::shared_ptr<LoadState>& state, int priority) {
    pool_.submit(priority, [this, path, state] {
        if (state->claimed.exchange(true)) return; // the promoted twin of this entry already ran it
        runLoad(path, state);
    });
}

bool AssetManager::cancel(const std::string& path) {
    std::vector<Callback> waiters;
    std::shared_ptr<LoadState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = inflight_.find(path);
        if (it == inflight_.end()) return false;
        // Whoever flips `claimed` first owns the load. If a worker already did, it is running: too late.
        if (it->second.state->claimed.exchange(true)) return false;
        state = it->second.state;
        waiters = std::move(it->second.waiters);
        inflight_.erase(it);
    }
    ++cancelled_;
    const std::exception_ptr error = std::make_exception_ptr(LoadCancelled());
    for (Callback& cb : waiters) pushCompletion(std::move(cb), Result{path, nullptr, error});
    state->promise.set_exception(error);
    return true;
}

void AssetManager::runLoad(const std::string& path, const std::shared_ptr<LoadState>& state) {
    ++active_;
    const auto began = Clock::now();
    AssetPtr asset;
    std::exception_ptr error;
    try {
        asset = decodeAsset(path, source_->read(path));
        cache_.put(path, asset, asset->cost()); // no-op if the asset is larger than the whole budget
    } catch (...) {
        error = std::current_exception();
        asset = nullptr;
    }

    std::vector<Callback> waiters;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = inflight_.find(path);
        waiters = std::move(it->second.waiters);
        inflight_.erase(it);
    }
    if (error) ++failed_; else ++completed_;
    for (Callback& cb : waiters) pushCompletion(std::move(cb), Result{path, asset, error});

    {
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - state->queuedAt).count();
        std::lock_guard<std::mutex> lock(latencyMutex_);
        constexpr std::size_t Window = 512;
        if (latenciesMs_.size() < Window) latenciesMs_.push_back(ms);
        else latenciesMs_[latencyNext_++ % Window] = ms;
    }
    busyNanos_ += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - began).count());
    --active_;
    if (error) state->promise.set_exception(error); else state->promise.set_value(asset);
}

void AssetManager::pushCompletion(Callback cb, Result result) {
    std::lock_guard<std::mutex> lock(doneMutex_);
    done_.emplace_back(std::move(cb), std::move(result));
}

std::size_t AssetManager::pumpCompletions() {
    std::deque<std::pair<Callback, Result>> batch;
    {
        std::lock_guard<std::mutex> lock(doneMutex_);
        batch.swap(done_);
    }
    for (auto& item : batch) item.first(item.second);
    return batch.size();
}

Stats AssetManager::stats() const {
    Stats s;
    s.requests = requests_;
    s.cacheHits = hits_;
    s.coalesced = coalesced_;
    s.loadsStarted = started_;
    s.loadsCompleted = completed_;
    s.loadsFailed = failed_;
    s.cancelled = cancelled_;
    s.busyMs = static_cast<double>(busyNanos_.load()) / 1e6;
    s.boosts = boosts_;
    s.evictions = cache_.evictions();
    s.cachedBytes = cache_.bytes();
    s.cachedAssets = cache_.size();
    s.cacheCapacity = cache_.capacity();
    s.workers = pool_.threadCount();
    s.activeLoads = active_;
    const std::uint64_t unfinished = s.loadsStarted - std::min(s.loadsStarted, s.loadsCompleted + s.loadsFailed + s.cancelled);
    s.queuedLoads = unfinished > s.activeLoads ? static_cast<std::size_t>(unfinished - s.activeLoads) : 0;
    std::vector<double> window;
    {
        std::lock_guard<std::mutex> lock(latencyMutex_);
        window = latenciesMs_;
    }
    if (!window.empty()) {
        std::sort(window.begin(), window.end());
        double sum = 0;
        for (double v : window) sum += v;
        s.meanLatencyMs = sum / static_cast<double>(window.size());
        s.p95LatencyMs = window[static_cast<std::size_t>(0.95 * static_cast<double>(window.size() - 1))];
        s.maxLatencyMs = window.back();
    }
    return s;
}

} // namespace streamer
