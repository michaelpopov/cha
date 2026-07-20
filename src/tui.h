#pragma once

#include "conversation.h"
#include "input_editor.h"
#include "transcript_renderer.h"

#include <curses.h>

#include <cstddef>
#include <string>

namespace cha {

class Terminal;

// Owns curses resources and renders the chat transcript, status line, and input editor.
class Tui {
public:
    explicit Tui(Terminal& terminal);
    ~Tui();

    Tui(const Tui&) = delete;
    Tui& operator=(const Tui&) = delete;

    [[nodiscard]] int read_key(wint_t& key);
    void render(const Conversation& conversation, const InputEditor& editor, bool generating, std::string_view notice = {});
    void scroll_up();
    void scroll_down();
    void resize();

private:
    void replace_pad(WINDOW*& pad, int rows, int columns);
    void ensure_transcript_capacity(int required_rows);
    void ensure_input_pad(int required_rows, int columns);
    void rebuild_transcript(const ConversationSnapshot& snapshot, int output_height, int columns);
    void write_transcript_entry(const ConversationMessage& message);
    void render_transcript(const ConversationSnapshot& snapshot, int output_height, int columns);
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
