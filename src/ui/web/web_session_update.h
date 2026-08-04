#pragma once

#include "session/session_change.h"

#include <optional>
#include <string>
#include <utility>

namespace cha::web {

// Temporary Block 5 compatibility bridge for the existing web runtime seam.
// Block 6 removes this after migrating web fakes and tests to SessionChange.
struct WebSessionUpdate {
    bool render_needed{};
    bool end_session{};
    bool clear_input{};
    std::optional<std::string> notice;
};

struct WebSessionEventBatch {
    WebSessionUpdate update;
    bool full{};
};

// Compatibility spellings retained only below ui/web until Block 6 migrates
// the runtime fake and test surface.
using SessionUpdate = WebSessionUpdate;
using SessionEventBatch = WebSessionEventBatch;

[[nodiscard]] inline WebSessionUpdate to_web_session_update(
    SessionChange change,
    bool clear_input = false) {
    return {
        .render_needed = change.state_changed,
        .end_session = change.controller_ended,
        .clear_input = clear_input,
        .notice = std::move(change.notice),
    };
}

} // namespace cha::web
