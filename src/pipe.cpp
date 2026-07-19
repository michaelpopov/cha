#include "pipe.h"

#include <cerrno>
#include <cstdint>
#include <poll.h>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>

namespace cha {

Pipe::Pipe() {
    notification_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    if (notification_fd_ == -1) {
        throw std::system_error(errno, std::generic_category(), "Failed to create pipe notification descriptor");
    }
}

Pipe::~Pipe() {
    ::close(notification_fd_);
}

void Pipe::put(std::string_view str) {
    push(PipeEvent{PipeEventKind::data, std::string(str)});
}

void Pipe::conversation_updated() {
    push(PipeEvent{PipeEventKind::conversation_updated, {}});
}

void Pipe::model_changed(std::string_view model) {
    push(PipeEvent{PipeEventKind::model_changed, std::string(model)});
}

void Pipe::push(PipeEvent event) {
    {
        std::lock_guard lock(mutex_);

        if (closed_) {
            return;
        }

        messages_.push(std::move(event));
        notify();
    }
}

PipeEvent Pipe::get() {
    wait_for_notification();
    std::lock_guard lock(mutex_);
    PipeEvent event = std::move(messages_.front());
    messages_.pop();
    return event;
}

bool Pipe::try_get(PipeEvent& event) {
    if (!try_consume_notification()) {
        return false;
    }

    std::lock_guard lock(mutex_);
    event = std::move(messages_.front());
    messages_.pop();
    return true;
}

void Pipe::eom() {
    {
        std::lock_guard lock(mutex_);

        if (closed_) {
            return;
        }

        messages_.push(PipeEvent{PipeEventKind::eom, {}});
        notify();
    }
}

void Pipe::close() {
    {
        std::lock_guard lock(mutex_);

        if (closed_) {
            return;
        }

        closed_ = true;
        messages_.push(PipeEvent{PipeEventKind::closed, {}});
        notify();
    }
}

int Pipe::notification_fd() const {
    return notification_fd_;
}

void Pipe::notify() const {
    eventfd_t value = 1;
    while (::eventfd_write(notification_fd_, value) == -1 && errno == EINTR) {
    }
}

void Pipe::wait_for_notification() const {
    pollfd descriptor{notification_fd_, POLLIN, 0};
    while (true) {
        const int result = ::poll(&descriptor, 1, -1);
        if (result == -1 && errno == EINTR) {
            continue;
        }
        if (result == -1) {
            throw std::system_error(errno, std::generic_category(), "Failed to wait for pipe notification");
        }
        if ((descriptor.revents & POLLIN) != 0) {
            if (try_consume_notification()) {
                return;
            }
            descriptor.revents = 0;
            continue;
        }
        throw std::runtime_error("Pipe notification descriptor became unavailable");
    }
}

bool Pipe::try_consume_notification() const {
    eventfd_t value = 0;
    while (::eventfd_read(notification_fd_, &value) == -1) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            return false;
        }
        throw std::system_error(errno, std::generic_category(), "Failed to consume pipe notification");
    }
    return true;
}

} // namespace cha
