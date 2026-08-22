#pragma once

#include <functional>
#include <string>
#include <string_view>

namespace cha {

// Buffers an inbound SSE byte stream and passes each non-empty `data:` line to
// the handler. Returning false stops the current read and discards its
// remaining bytes; callers retain ownership of terminal-event policy.
class SseFramer {
public:
    using DataHandler = std::function<bool(std::string_view)>;

    void consume(std::string_view bytes, const DataHandler& on_data);
    void finish(const DataHandler& on_data);

private:
    bool read_event(std::string_view event, const DataHandler& on_data);

    std::string pending_;
};

} // namespace cha
