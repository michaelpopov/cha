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
    CompletionDeltaKind suffix_kind{CompletionDeltaKind::answer};
    std::string last_message_suffix;
    std::size_t first_new_message{};
};

enum class TranscriptAttributes {
    normal,
    bold,
    dim,
    bold_dim,
};

class TranscriptSurface {
public:
    virtual ~TranscriptSurface() = default;
    virtual void attributes(TranscriptAttributes value) = 0;
    virtual void write(std::string_view text) = 0;
};

// Writes one entry's labeled content and always restores normal attributes.
void write_transcript_entry(
    TranscriptSurface& surface,
    const ConversationEntry& entry,
    bool show_addressing);
void write_transcript_suffix(
    TranscriptSurface& surface,
    CompletionDeltaKind kind,
    std::string_view text);
void initialize_transcript_surface(TranscriptSurface& surface);

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
    std::size_t entry_count_{};
    std::optional<ConversationEntry> last_entry_;
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

// Produces an unambiguous display label from an entry's semantic kind.
[[nodiscard]] std::string transcript_entry_label(const ConversationEntry& entry, bool show_addressing);

// Conservatively estimates pad rows for UTF-8 terminal text.
[[nodiscard]] int layout_rows(std::string_view text, int columns, int initial_cells = 0);

// Estimates pad rows for wide-character terminal input.
[[nodiscard]] int layout_rows(std::wstring_view text, int columns, int initial_cells = 0);

} // namespace cha
