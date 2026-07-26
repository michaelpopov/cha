#pragma once

#include "transcript/transcript.h"
#include "ui/tui/input_editor.h"
#include "ui/tui/session_view.h"
#include "ui/render/transcript_writer.h"
#include "ui/tui/render_plan.h"
#include "ui/tui/screen_layout.h"

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
        const Transcript& transcript,
        const InputEditor& editor,
        const GenerationStatus& status,
        bool show_addressing,
        std::string_view input_target_name,
        std::string_view notice = {}) override;
    void scroll_up() override;
    void scroll_down() override;
    void resize() override;

private:
    void replace_pad(WINDOW*& pad, int rows, int columns);
    void ensure_transcript_capacity(int required_rows);
    void ensure_input_pad(int required_rows, int columns);
    void rebuild_transcript(
        const TranscriptSnapshot& snapshot,
        const GenerationStatus& status,
        int output_height,
        int columns,
        bool show_addressing);
    void write_transcript_entry(const TranscriptEntry& entry, bool show_addressing);
    void write_active_response(
        const GenerationStatus& status,
        std::string_view answer_text);
    void render_transcript(
        const TranscriptSnapshot& snapshot,
        const GenerationStatus& status,
        int output_height,
        int columns,
        bool show_addressing);
    void render_input(
        const InputEditor& editor,
        std::string_view input_target_name,
        int input_y,
        int input_height,
        int columns);

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
    GenerationStatus rendered_generation_;
    Terminal& terminal_;
};

} // namespace cha
