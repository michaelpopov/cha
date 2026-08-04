#pragma once

#include <optional>
#include <string>

namespace cha {

// Semantic outcome of one owner-thread controller operation. Frontends decide
// how to present notices and whether accepted text clears a local widget.
struct SessionChange {
    bool state_changed{};
    bool input_consumed{};
    bool controller_ended{};
    // nullopt leaves a frontend's current notice unchanged, an empty string
    // clears it, and a non-empty string replaces it.
    std::optional<std::string> notice;
};

// A bounded agent-event drain. A full batch is deliberately conservative: it
// tells an owner loop to try another drain before sleeping, even when the last
// event happened to empty the channel.
struct SessionEventBatch {
    SessionChange change;
    bool full{};
};

} // namespace cha
