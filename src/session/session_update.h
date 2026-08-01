#pragma once

#include <optional>
#include <string>

namespace cha {

// The side effects one controller command leaves for the front end to apply.
// Returning them keeps every UI type out of the controller.
struct SessionUpdate {
    bool render_needed{};
    bool end_session{};
    bool clear_input{};
    // nullopt leaves the frontend's current notice unchanged, an empty string
    // clears it, and a non-empty string replaces it.
    std::optional<std::string> notice;
};

// A bounded agent-event drain. A full batch is deliberately conservative: it
// tells an owner loop to try another drain before sleeping, even when the last
// event happened to empty the channel.
struct SessionEventBatch {
    SessionUpdate update;
    bool full{};
};

} // namespace cha
