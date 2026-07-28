#pragma once

#include "transcript/transcript.h"

#include <cstddef>
#include <optional>
#include <string_view>

namespace cha {

enum class TranscriptRenderAction {
    none,
    rebuild,
    append,
};

struct TranscriptRenderPlan {
    TranscriptRenderAction action{TranscriptRenderAction::none};
    bool resumes_last_message{};
    // Borrows from the TranscriptView passed to plan(). Consume it before the
    // owning Transcript is mutated or destroyed; copying the plan does not
    // extend that lifetime.
    std::string_view last_message_suffix;
    std::size_t first_new_message{};
};

// Retains only scalar positions between frames. This relies on Transcript's
// mutation contract: committed entries are immutable, and only the open tail
// may grow before it is finished or discarded.
class TranscriptRenderPlanner {
public:
    TranscriptRenderPlan plan(TranscriptView transcript, int columns) const;
    void commit(TranscriptView transcript, int columns);

private:
    bool initialized_{};
    int columns_{};
    std::size_t revision_{};
    std::size_t history_epoch_{};
    std::size_t entry_count_{};
    std::optional<EntryId> last_entry_id_;
    std::size_t last_text_size_{};
};

} // namespace cha
