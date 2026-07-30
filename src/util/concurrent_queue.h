#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace cha {

// Reports whether a non-blocking queue read found a value, no value, or a
// closed and drained queue.
enum class ChannelReadStatus {
    value,
    empty,
    closed,
};

// A small unbounded queue for the application's concurrent channels.
// Closing wakes blocked readers and preserves values that were already queued.
// close_with() additionally reserves one final value without appending it to
// the potentially allocating deque; readers receive it after queued values.
template <typename T>
class ConcurrentQueue {
public:
    ConcurrentQueue() = default;
    ConcurrentQueue(const ConcurrentQueue&) = delete;
    ConcurrentQueue& operator=(const ConcurrentQueue&) = delete;

    [[nodiscard]] bool push(T value) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return false;
            }
            values_.push_back(std::move(value));
        }
        ready_.notify_one();
        return true;
    }

    std::optional<T> get() {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [this] {
            return closed_ || !values_.empty();
        });
        if (values_.empty()) {
            if (!close_value_) {
                return std::nullopt;
            }
            T value = std::move(*close_value_);
            close_value_.reset();
            return value;
        }
        T value = std::move(values_.front());
        values_.pop_front();
        return value;
    }

    ChannelReadStatus try_get(T& value) {
        std::lock_guard lock(mutex_);
        if (values_.empty()) {
            if (close_value_) {
                value = std::move(*close_value_);
                close_value_.reset();
                return ChannelReadStatus::value;
            }
            return closed_
                ? ChannelReadStatus::closed
                : ChannelReadStatus::empty;
        }
        value = std::move(values_.front());
        values_.pop_front();
        return ChannelReadStatus::value;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
        }
        ready_.notify_all();
    }

    // Closes the queue with one final value. The first close operation wins.
    // Moving into the reserved slot does not allocate queue storage.
    void close_with(T value) {
        {
            std::lock_guard lock(mutex_);
            if (closed_) {
                return;
            }
            close_value_.emplace(std::move(value));
            closed_ = true;
        }
        ready_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<T> values_;
    std::optional<T> close_value_;
    bool closed_{};
};

} // namespace cha
