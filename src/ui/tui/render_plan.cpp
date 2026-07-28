#include "ui/tui/render_plan.h"

namespace cha {

TranscriptRenderPlan TranscriptRenderPlanner::plan(
    TranscriptView transcript,
    int columns
) const {
    if (!initialized_ || columns != columns_
        || transcript.history_epoch != history_epoch_
        || transcript.size() < entry_count_) {
        return {.action = TranscriptRenderAction::rebuild};
    }

    if (transcript.revision == revision_) {
        return {};
    }

    if (last_entry_id_) {
        const TranscriptEntry& previous_tail =
            transcript.entries[entry_count_ - 1];
        if (previous_tail.id != *last_entry_id_
            || previous_tail.text.size() < last_text_size_) {
            return {.action = TranscriptRenderAction::rebuild};
        }
        if (transcript.size() == entry_count_
            && previous_tail.text.size() == last_text_size_) {
            return {};
        }

        return {
            .action = TranscriptRenderAction::append,
            .resumes_last_message = true,
            .last_message_suffix =
                std::string_view(previous_tail.text).substr(last_text_size_),
            .first_new_message = entry_count_,
        };
    }

    return {
        .action = TranscriptRenderAction::append,
        .first_new_message = 0,
    };
}

void TranscriptRenderPlanner::commit(
    TranscriptView transcript,
    int columns) {
    initialized_ = true;
    columns_ = columns;
    revision_ = transcript.revision;
    history_epoch_ = transcript.history_epoch;
    entry_count_ = transcript.size();
    if (transcript.empty()) {
        last_entry_id_.reset();
        last_text_size_ = 0;
    } else {
        last_entry_id_ = transcript.entries.back().id;
        last_text_size_ = transcript.entries.back().text.size();
    }
}

} // namespace cha
