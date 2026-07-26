#pragma once

namespace cha {

// Wakes the front-end event loop after agent-event queue state changes.
class WakeNotifier {
public:
    virtual ~WakeNotifier() = default;
    virtual void wake() noexcept = 0;
};

} // namespace cha
