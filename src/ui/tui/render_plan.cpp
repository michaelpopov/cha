#include "ui/tui/render_plan.h"

namespace cha {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size()
        && text.substr(0, prefix.size()) == prefix;
}

} // namespace

TranscriptRenderPlan TranscriptRenderPlanner::plan(
    const TranscriptSnapshot& snapshot,
    int columns
) const {
    if (!initialized_ || columns != columns_
        || snapshot.history_epoch != history_epoch_
        || snapshot.entries.size() < entry_count_) {
        return {.action = TranscriptRenderAction::rebuild};
    }

    if (snapshot.revision == revision_) {
        return {};
    }

    if (last_entry_) {
        const TranscriptEntry& old_last = *last_entry_;
        const TranscriptEntry& new_last = snapshot.entries[entry_count_ - 1];
        if (snapshot.entries.size() == entry_count_ && new_last == old_last) {
            return {};
        }
        if (new_last.id != old_last.id
            || new_last.kind != old_last.kind
            || new_last.participant_id != old_last.participant_id
            || new_last.display_name != old_last.display_name
            || new_last.addressed_to != old_last.addressed_to
            || new_last.addressed_to_name != old_last.addressed_to_name
            || new_last.request_id != old_last.request_id
            || !starts_with(new_last.text, old_last.text)) {
            return {.action = TranscriptRenderAction::rebuild};
        }

        return {
            .action = TranscriptRenderAction::append,
            .resumes_last_message = true,
            .last_message_suffix = new_last.text.substr(old_last.text.size()),
            .first_new_message = entry_count_,
        };
    }

    return {
        .action = TranscriptRenderAction::append,
        .first_new_message = 0,
    };
}

void TranscriptRenderPlanner::commit(
    const TranscriptSnapshot& snapshot,
    int columns) {
    initialized_ = true;
    columns_ = columns;
    revision_ = snapshot.revision;
    history_epoch_ = snapshot.history_epoch;
    entry_count_ = snapshot.entries.size();
    if (snapshot.entries.empty()) {
        last_entry_.reset();
    } else {
        last_entry_ = snapshot.entries.back();
    }
}

} // namespace cha
