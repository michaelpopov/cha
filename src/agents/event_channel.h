#pragma once

#include <cerrno>
#include <deque>
#include <mutex>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>

namespace cha {

// Reports whether a non-blocking channel read found a value, no value, or a closed channel.
enum class ChannelReadStatus {
    value,
    empty,
    closed,
};

// Provides a typed, thread-safe queue whose descriptor can participate in the terminal event loop.
template <typename T>
class EventChannel {
public:
    EventChannel() {
        notification_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
        if (notification_fd_ == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to create channel notification descriptor");
        }
    }

    ~EventChannel() {
        ::close(notification_fd_);
    }

    EventChannel(const EventChannel&) = delete;
    EventChannel& operator=(const EventChannel&) = delete;

    // Enqueues one semantic value, returning false when the channel is already closed.
    [[nodiscard]] bool push(T value) {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return false;
        }

        values_.push_back(std::move(value));
        try {
            notify();
        } catch (...) {
            values_.pop_back();
            throw;
        }
        return true;
    }

    // Waits for one value; after queued values drain, a closed channel keeps returning nullopt.
    std::optional<T> get() {
        while (true) {
            {
                std::lock_guard lock(mutex_);
                if (values_.empty() && closed_) {
                    return std::nullopt;
                }
            }

            wait_for_notification();
            std::lock_guard lock(mutex_);
            if (!values_.empty()) {
                T value = std::move(values_.front());
                values_.pop_front();
                return value;
            }
        }
    }

    // Attempts one read without blocking and distinguishes an empty channel from a closed one.
    ChannelReadStatus try_get(T& value) {
        {
            std::lock_guard lock(mutex_);
            if (values_.empty() && closed_) {
                return ChannelReadStatus::closed;
            }
        }

        if (!try_consume_notification()) {
            return ChannelReadStatus::empty;
        }

        std::lock_guard lock(mutex_);
        if (values_.empty()) {
            return closed_ ? ChannelReadStatus::closed : ChannelReadStatus::empty;
        }
        value = std::move(values_.front());
        values_.pop_front();
        return ChannelReadStatus::value;
    }

    // Prevents new writes and wakes blocked readers while preserving already queued values.
    void close() {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        notify();
        closed_ = true;
    }

    int notification_fd() const {
        return notification_fd_;
    }

private:
    void notify() const {
        eventfd_t value = 1;
        while (::eventfd_write(notification_fd_, value) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                // The counter is saturated, so the descriptor is already
                // readable and a notification is pending.
                return;
            }
            throw std::system_error(errno, std::generic_category(), "Failed to signal channel notification");
        }
    }

    void wait_for_notification() const {
        pollfd descriptor{notification_fd_, POLLIN, 0};
        while (true) {
            const int result = ::poll(&descriptor, 1, -1);
            if (result == -1 && errno == EINTR) {
                continue;
            }
            if (result == -1) {
                throw std::system_error(errno, std::generic_category(), "Failed to wait for channel notification");
            }
            if ((descriptor.revents & POLLIN) != 0 && try_consume_notification()) {
                return;
            }
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                throw std::runtime_error("Channel notification descriptor became unavailable");
            }
            descriptor.revents = 0;
        }
    }

    bool try_consume_notification() const {
        eventfd_t value = 0;
        while (::eventfd_read(notification_fd_, &value) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return false;
            }
            throw std::system_error(errno, std::generic_category(), "Failed to read channel notification");
        }
        return true;
    }

    bool closed_{};
    mutable std::mutex mutex_;
    std::deque<T> values_;
    int notification_fd_{-1};
};

} // namespace cha
