#pragma once

#include "conversation.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace cha {

// Describes whether a transcript adapter can reuse its current output or must rebuild it.
enum class TranscriptRenderAction {
    none,
    rebuild,
    append,
};

// Identifies the content an adapter must replay for one transcript update.
struct TranscriptRenderPlan {
    TranscriptRenderAction action{TranscriptRenderAction::none};
    bool resumes_last_message{};
    std::string last_message_suffix;
    std::size_t first_new_message{};
};

// Tracks rendered conversation state and computes safe incremental update plans.
class TranscriptRenderPlanner {
public:
    [[nodiscard]] TranscriptRenderPlan plan(const ConversationSnapshot& snapshot, int columns) const;
    void commit(const ConversationSnapshot& snapshot, int columns);

private:
    bool initialized_{};
    int columns_{};
    std::size_t revision_{};
    std::size_t history_epoch_{};
    std::size_t message_count_{};
    std::optional<ConversationMessage> last_message_;
};

// Maintains transcript scrolling independently of terminal rendering calls.
class TranscriptViewport {
public:
    void update(int transcript_lines, int output_height);
    void scroll_up();
    void scroll_down();

    [[nodiscard]] int top() const;
    [[nodiscard]] bool follows_output() const;

private:
    [[nodiscard]] int bottom() const;

    int transcript_lines_{};
    int output_height_{};
    int top_{};
    bool follows_output_{true};
};

// Conservatively estimates pad rows for UTF-8 terminal text.
[[nodiscard]] int layout_rows(std::string_view text, int columns, int initial_cells = 0);

// Estimates pad rows for wide-character terminal input.
[[nodiscard]] int layout_rows(std::wstring_view text, int columns, int initial_cells = 0);

} // namespace cha
