#pragma once

#include "agents/agent_registry.h"
#include "conversation/conversation.h"

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

// What a renderer must do to catch up with the newest snapshot: nothing, append the new messages
// and the text streamed onto the last one, or rebuild the transcript from scratch.
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

// A sink for attributed transcript text. It keeps entry formatting independent of curses, so the
// writers below serve both Tui and tests that simply record what would have been drawn.
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

// Remembers what has already been drawn and turns each new ConversationSnapshot into a
// TranscriptRenderPlan, so streamed output can be appended instead of repainting everything. A
// width change, a history reset, or any change that cannot be expressed as appended text forces a
// rebuild. A plan holds until commit() records the snapshot the renderer actually drew.
class TranscriptRenderPlanner {
public:
    TranscriptRenderPlan plan(const ConversationSnapshot& snapshot, int columns) const;
    void commit(const ConversationSnapshot& snapshot, int columns);

private:
    bool initialized_{};
    int columns_{};
    std::size_t revision_{};
    std::size_t history_epoch_{};
    std::size_t entry_count_{};
    std::optional<ConversationEntry> last_entry_;
};

// The scroll position of the transcript window, kept apart from drawing so scrolling is testable.
// It tracks the top row and whether the view still follows new output, so a user who scrolls back
// is not dragged forward by arriving text.
class TranscriptViewport {
public:
    void update(int transcript_lines, int output_height);
    void scroll_up();
    void scroll_down();

    int top() const;
    bool follows_output() const;

private:
    int bottom() const;

    int transcript_lines_{};
    int output_height_{};
    int top_{};
    bool follows_output_{true};
};

// Whether transcript labels should include routing (multi-agent room or foreign history).
bool show_addressing(
    const AgentRoster& roster,
    const Conversation& conversation);

// Produces an unambiguous display label from an entry's semantic kind.
std::string transcript_entry_label(const ConversationEntry& entry, bool show_addressing);

// Conservatively estimates pad rows for UTF-8 terminal text.
int layout_rows(std::string_view text, int columns, int initial_cells = 0);

// Estimates pad rows for wide-character terminal input.
int layout_rows(std::wstring_view text, int columns, int initial_cells = 0);

} // namespace cha
