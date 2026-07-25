#pragma once

#include "conversation/conversation.h"
#include "ui/terminal/input_editor.h"
#include "ui/terminal/session_view.h"
#include "ui/terminal/transcript_renderer.h"

#include <curses.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace cha {

class Terminal;

// The curses implementation of SessionView. It owns the transcript and input pads drawn on a
// borrowed Terminal, decodes key presses into SessionInput values, and paints the transcript,
// status line, and editor, leaning on TranscriptRenderPlanner and TranscriptViewport to redraw
// incrementally and to keep scrolling stable while output streams in.
class Tui : public SessionView {
public:
    explicit Tui(Terminal& terminal);
    ~Tui() override;

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    std::optional<SessionInput> read_input() override;
    void render(
        const Conversation& conversation,
        const InputEditor& editor,
        const GenerationStatus& status,
        bool show_addressing,
        std::string_view notice = {}) override;
    void scroll_up() override;
    void scroll_down() override;
    void resize() override;

private:
    void replace_pad(WINDOW*& pad, int rows, int columns);
    void ensure_transcript_capacity(int required_rows);
    void ensure_input_pad(int required_rows, int columns);
    void rebuild_transcript(const ConversationSnapshot& snapshot, int output_height, int columns, bool show_addressing);
    void write_transcript_entry(const ConversationEntry& entry, bool show_addressing);
    void render_transcript(const ConversationSnapshot& snapshot, int output_height, int columns, bool show_addressing);
    void render_input(const InputEditor& editor, int input_y, int input_height, int columns);

    WINDOW* transcript_pad_{};
    WINDOW* input_pad_{};
    int rendered_last_content_y_{};
    int rendered_last_content_x_{};
    int transcript_capacity_{};
    int transcript_columns_{};
    int input_capacity_{};
    int input_columns_{};
    TranscriptRenderPlanner transcript_planner_;
    TranscriptViewport transcript_viewport_;
    Terminal& terminal_;
};

} // namespace cha
