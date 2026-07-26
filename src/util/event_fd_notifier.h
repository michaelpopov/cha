#pragma once

#include "util/wake_notifier.h"

namespace cha {

// Linux event-loop wake adapter. Wakes may be coalesced; acknowledge() clears
// the accumulated eventfd counter before the consumer drains its queue.
class EventFdNotifier final : public WakeNotifier {
public:
    EventFdNotifier();
    ~EventFdNotifier() override;

    EventFdNotifier(const EventFdNotifier&) = delete;
    EventFdNotifier& operator=(const EventFdNotifier&) = delete;

    void wake() noexcept override;
    int descriptor() const noexcept;
    void acknowledge() noexcept;

private:
    int descriptor_{-1};
};

} // namespace cha
