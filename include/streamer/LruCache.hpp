#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace streamer {

// Thread-safe least-recently-used cache with a byte budget. Values are shared_ptr<const V>, so an
// entry evicted while a caller still holds it stays alive until that caller drops it: eviction
// never invalidates memory that is in use.
template <class V>
class LruCache {
public:
    using Ptr = std::shared_ptr<const V>;

    explicit LruCache(std::size_t capacityBytes) : capacity_(capacityBytes) {}

    // Returns the value and marks it most recently used, or nullptr on a miss.
    Ptr get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = index_.find(key);
        if (it == index_.end()) return nullptr;
        order_.splice(order_.begin(), order_, it->second);
        return it->second->value;
    }

    // Inserts or replaces `key`, then evicts least-recently-used entries until the budget holds.
    // Returns false (and caches nothing) if `bytes` alone exceeds the whole budget.
    bool put(const std::string& key, Ptr value, std::size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto existing = index_.find(key);
        if (existing != index_.end()) {
            usedBytes_ -= existing->second->bytes;
            order_.erase(existing->second);
            index_.erase(existing);
        }
        if (bytes > capacity_) return false;
        order_.push_front(Entry{key, std::move(value), bytes});
        index_[key] = order_.begin();
        usedBytes_ += bytes;
        while (usedBytes_ > capacity_) {
            const Entry& victim = order_.back();
            usedBytes_ -= victim.bytes;
            index_.erase(victim.key);
            order_.pop_back();
            ++evictions_;
        }
        return true;
    }

    std::size_t capacity() const { return capacity_; }
    std::size_t bytes() const { return locked([this] { return usedBytes_; }); }
    std::size_t size() const { return locked([this] { return index_.size(); }); }
    std::uint64_t evictions() const { return locked([this] { return evictions_; }); }

private:
    struct Entry {
        std::string key;
        Ptr value;
        std::size_t bytes;
    };

    template <class F>
    auto locked(F&& f) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return f();
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::list<Entry> order_; // front = most recently used
    std::unordered_map<std::string, typename std::list<Entry>::iterator> index_;
    std::size_t usedBytes_ = 0;
    std::uint64_t evictions_ = 0;
};

} // namespace streamer
