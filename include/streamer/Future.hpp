#pragma once

// A small promise / shared-future pair built on std::mutex + std::condition_variable.
//
// Why not std::promise / std::shared_future? They work, but their hand-off goes through libstdc++'s
// internal call_once / futex machinery, which ThreadSanitizer does not model: every value passed from
// a worker to a waiter was reported as a data race (147 reports on the GitHub runner, 65/65 tests
// passing otherwise). Plain mutex and condition-variable operations are fully understood by TSan, so
// with this type a clean TSan run means something.
//
//   Promise<T>        write side; set_value / set_exception exactly once (a second call throws).
//   SharedFuture<T>   read side; cheap to copy, any number of threads may get() / wait() on copies.
//
// Semantics match std::shared_future closely enough for this project: get() blocks until the value is
// set, rethrows a stored exception, and returns a const reference that stays valid while any copy of
// the future lives. A Promise destroyed without a value fails its futures with std::runtime_error
// instead of leaving waiters blocked forever.
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future> // std::future_status only
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace streamer {

namespace detail {

struct Unit {};

template <class T>
struct GetResult { using type = const T&; };
template <>
struct GetResult<void> { using type = void; };

template <class T>
using Stored = std::conditional_t<std::is_void_v<T>, Unit, T>;

template <class T>
struct SharedState {
    mutable std::mutex mutex;
    mutable std::condition_variable cv;
    bool ready = false;
    std::optional<Stored<T>> value;
    std::exception_ptr error;

    void finish() { // caller holds `mutex`
        ready = true;
        cv.notify_all();
    }
};

} // namespace detail

template <class T>
class SharedFuture {
public:
    SharedFuture() = default;

    bool valid() const { return static_cast<bool>(state_); }

    void wait() const {
        check();
        std::unique_lock<std::mutex> lock(state_->mutex);
        state_->cv.wait(lock, [this] { return state_->ready; });
    }

    template <class Rep, class Period>
    std::future_status wait_for(const std::chrono::duration<Rep, Period>& timeout) const {
        check();
        std::unique_lock<std::mutex> lock(state_->mutex);
        return state_->cv.wait_for(lock, timeout, [this] { return state_->ready; }) ? std::future_status::ready
                                                                                    : std::future_status::timeout;
    }

    // Blocks until ready; rethrows the stored exception, otherwise returns the value.
    typename detail::GetResult<T>::type get() const {
        wait();
        if (state_->error) std::rethrow_exception(state_->error);
        if constexpr (!std::is_void_v<T>) return *state_->value;
    }

private:
    template <class>
    friend class Promise;
    explicit SharedFuture(std::shared_ptr<detail::SharedState<T>> s) : state_(std::move(s)) {}
    void check() const {
        if (!state_) throw std::logic_error("SharedFuture has no shared state");
    }
    std::shared_ptr<detail::SharedState<T>> state_;
};

template <class T>
class Promise {
public:
    Promise() : state_(std::make_shared<detail::SharedState<T>>()) {}
    Promise(Promise&&) noexcept = default;
    Promise& operator=(Promise&&) = delete;
    Promise(const Promise&) = delete;
    Promise& operator=(const Promise&) = delete;

    ~Promise() {
        if (!state_) return;
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->ready) {
            state_->error = std::make_exception_ptr(std::runtime_error("promise destroyed without a value"));
            state_->finish();
        }
    }

    SharedFuture<T> future() const { return SharedFuture<T>(state_); }

    template <class U = T, class = std::enable_if_t<!std::is_void_v<U>>>
    void set_value(U&& v) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        claim();
        state_->value.emplace(std::forward<U>(v));
        state_->finish();
    }
    template <class U = T, class = std::enable_if_t<std::is_void_v<U>>>
    void set_value() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        claim();
        state_->value.emplace();
        state_->finish();
    }

    void set_exception(std::exception_ptr e) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        claim();
        state_->error = std::move(e);
        state_->finish();
    }

private:
    void claim() const { // caller holds the lock
        if (state_->ready) throw std::logic_error("Promise already satisfied");
    }
    std::shared_ptr<detail::SharedState<T>> state_;
};

} // namespace streamer
