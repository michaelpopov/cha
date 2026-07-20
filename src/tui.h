#pragma once

#include "conversation.h"
#include "input_editor.h"

#include <curses.h>

#include <cstddef>
#include <string>

namespace cha {

// Own curses resources and rendering policy so the session remains independent of terminal mechanics.
class Tui {
public:
    explicit Tui();
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
    void rebuild_transcript(const ConversationSnapshot& snapshot, int output_height, int columns);
    void write_transcript_entry(const ConversationMessage& message);
    void render_transcript(const ConversationSnapshot& snapshot, int output_height, int columns);
    void render_input(const InputEditor& editor, int input_y, int input_height, int columns);

    WINDOW* transcript_pad_{};
    WINDOW* input_pad_{};
    std::size_t rendered_revision_{};
    std::size_t rendered_entry_count_{};
    std::size_t rendered_last_text_size_{};
    int rendered_last_content_y_{};
    int rendered_last_content_x_{};
    int transcript_capacity_{};
    int transcript_columns_{};
    int transcript_lines_{};
    int view_top_{};
    int last_output_height_{};
    bool follow_output_{true};
};

} // namespace cha
