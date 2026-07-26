#include "util/event_fd_notifier.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>

namespace cha {
namespace {

[[noreturn]] void fatal_notifier_error(const char* action) noexcept {
    std::fputs("Fatal: failed to ", stderr);
    std::fputs(action, stderr);
    std::fputs(" event-loop notifier\n", stderr);
    std::abort();
}

} // namespace

EventFdNotifier::EventFdNotifier()
    : descriptor_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (descriptor_ == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to create event-loop notifier");
    }
}

EventFdNotifier::~EventFdNotifier() {
    if (descriptor_ != -1) {
        ::close(descriptor_);
    }
}

void EventFdNotifier::wake() noexcept {
    while (::eventfd_write(descriptor_, 1) == -1) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            return;
        }
        fatal_notifier_error("signal");
    }
}

int EventFdNotifier::descriptor() const noexcept {
    return descriptor_;
}

void EventFdNotifier::acknowledge() noexcept {
    eventfd_t value = 0;
    while (::eventfd_read(descriptor_, &value) == -1) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            return;
        }
        fatal_notifier_error("acknowledge");
    }
}

} // namespace cha
