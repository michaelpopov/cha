#include "session/controller_update.h"

#include <utility>

namespace cha {

ControllerStateUpdate merge_state(
    ControllerStateUpdate all,
    ControllerStateUpdate one) {
    if (std::holds_alternative<NoStateUpdate>(one)) return all;
    if (std::holds_alternative<NoStateUpdate>(all)) return one;
    if (std::holds_alternative<SnapshotRequired>(all)
        || std::holds_alternative<SnapshotRequired>(one)) {
        return SnapshotRequired{};
    }
    auto& first = std::get<TextAppend>(all);
    auto& second = std::get<TextAppend>(one);
    // Only one target can be represented by a single append event, so a target
    // switch inside one batch is conservatively a full snapshot.
    if (first.target != second.target) return SnapshotRequired{};
    first.text += second.text;
    return all;
}

void merge(ControllerUpdate& all, ControllerUpdate one) {
    all.state = merge_state(std::move(all.state), std::move(one.state));
    // Completion events never consume input, so this OR preserves a command-side
    // acceptance without letting a drained event manufacture one.
    all.input_consumed = all.input_consumed || one.input_consumed;
    all.session_ended = all.session_ended || one.session_ended;
    if (one.notice) {
        all.notice = std::move(one.notice);
    }
}

} // namespace cha
