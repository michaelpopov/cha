#pragma once

#include "transcript/transcript.h"

#include <cstddef>
#include <optional>
#include <string>

namespace cha {

enum class TranscriptRenderAction {
    none,
    rebuild,
    append,
};

struct TranscriptRenderPlan {
    TranscriptRenderAction action{TranscriptRenderAction::none};
    bool resumes_last_message{};
    std::string last_message_suffix;
    std::size_t first_new_message{};
};

class TranscriptRenderPlanner {
public:
    TranscriptRenderPlan plan(const TranscriptSnapshot& snapshot, int columns) const;
    void commit(const TranscriptSnapshot& snapshot, int columns);

private:
    bool initialized_{};
    int columns_{};
    std::size_t revision_{};
    std::size_t history_epoch_{};
    std::size_t entry_count_{};
    std::optional<TranscriptEntry> last_entry_;
};

} // namespace cha
