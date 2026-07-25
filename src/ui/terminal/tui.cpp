#include "ui/terminal/tui.h"

#include "ui/terminal/terminal.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {
namespace {

constexpr int input_height = 5;
constexpr int status_height = 1;

class CursesTranscriptSurface final : public TranscriptSurface {
public:
    explicit CursesTranscriptSurface(WINDOW* window) : window_(window) {}

    void attributes(TranscriptAttributes value) override {
        int attributes = A_NORMAL;
        switch (value) {
        case TranscriptAttributes::normal:
            break;
        case TranscriptAttributes::bold:
            attributes = A_BOLD;
            break;
        case TranscriptAttributes::dim:
            attributes = A_DIM;
            break;
        case TranscriptAttributes::bold_dim:
            attributes = A_BOLD | A_DIM;
            break;
        }
        wattrset(window_, attributes);
    }

    void write(std::string_view text) override {
        waddnstr(
            window_,
            text.data(),
            static_cast<int>(text.size()));
    }

private:
    WINDOW* window_{};
};

void write_status(std::string_view text, int row, int columns) {
    move(row, 0);

    std::mbstate_t state{};
    const char* bytes = text.data();
    std::size_t remaining = text.size();
    int column = 0;

    while (remaining > 0) {
        wchar_t character = L'\0';
        const std::size_t consumed = std::mbrtowc(&character, bytes, remaining, &state);
        if (consumed == static_cast<std::size_t>(-1) || consumed == static_cast<std::size_t>(-2)) {
            state = {};
            ++bytes;
            --remaining;
            continue;
        }
        if (consumed == 0) {
            break;
        }

        const int width = std::max(0, ::wcwidth(character));
        if (column + width > columns) {
            break;
        }

        waddnwstr(stdscr, &character, 1);
        column += width;
        bytes += consumed;
        remaining -= consumed;
    }

    while (column < columns) {
        addch(' ');
        ++column;
    }
}

bool active_answer_has_reasoning(
    const TranscriptSnapshot& snapshot,
    const GenerationStatus& status) {
    return status.active
        && status.phase == ResponsePhase::answering
        && !status.reasoning_text.empty()
        && snapshot.open_entry_id
        && !snapshot.entries.empty()
        && snapshot.entries.back().id == *snapshot.open_entry_id;
}

std::string active_response_layout_text(
    const GenerationStatus& status,
    std::string_view answer_text) {
    std::string text = "[" + status.agent_name + "]\n[Reasoning]\n"
        + status.reasoning_text;
    if (!answer_text.empty()) {
        text += "\n\n";
        text += answer_text;
    }
    return text + "\n\n";
}

} // namespace

Tui::Tui(Terminal& terminal) : terminal_(terminal) {
    terminal_.configure_chat();
}

Tui::~Tui() {
    if (transcript_pad_) {
        delwin(transcript_pad_);
    }
    if (input_pad_) {
        delwin(input_pad_);
    }
}

std::optional<SessionInput> Tui::read_input() {
    wint_t key = 0;
    const int result = wget_wch(stdscr, &key);
    if (result == ERR) {
        return std::nullopt;
    }
    if (key == KEY_RESIZE) {
        return SessionInput{SessionInputKind::resize};
    }
    if (key == KEY_PPAGE) {
        return SessionInput{SessionInputKind::page_up};
    }
    if (key == KEY_NPAGE) {
        return SessionInput{SessionInputKind::page_down};
    }
    if (key == KEY_LEFT) {
        return SessionInput{SessionInputKind::left};
    }
    if (key == KEY_RIGHT) {
        return SessionInput{SessionInputKind::right};
    }
    if (key == KEY_UP) {
        return SessionInput{SessionInputKind::up};
    }
    if (key == KEY_DOWN) {
        return SessionInput{SessionInputKind::down};
    }
    if (key == KEY_HOME) {
        return SessionInput{SessionInputKind::home};
    }
    if (key == KEY_END) {
        return SessionInput{SessionInputKind::end};
    }
    if (key == KEY_DC) {
        return SessionInput{SessionInputKind::erase};
    }
    if (key == KEY_BACKSPACE || key == 127 || key == L'\b') {
        return SessionInput{SessionInputKind::backspace};
    }
    if (key == 27) {
        return SessionInput{SessionInputKind::escape};
    }
    if (key == 3) {
        return SessionInput{SessionInputKind::interrupt};
    }
    if (key == L'\n' || key == L'\r' || key == KEY_ENTER) {
        return SessionInput{SessionInputKind::enter};
    }
    if (result == OK && std::iswprint(key) != 0) {
        return SessionInput{
            .kind = SessionInputKind::character,
            .character = static_cast<wchar_t>(key),
        };
    }
    return SessionInput{};
}

void Tui::render(
    const Transcript& transcript,
    const InputEditor& editor,
    const GenerationStatus& status,
    bool show_addressing,
    std::string_view notice) {
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);

    if (rows < input_height + status_height + 1 || columns < 20) {
        erase();
        mvaddnstr(0, 0, "Terminal is too small", std::max(0, columns));
        refresh();
        return;
    }

    const int output_height = rows - input_height - status_height;
    const int status_y = output_height;
    const int input_y = status_y + status_height;
    erase();

    std::string phase = "generating";
    if (status.phase == ResponsePhase::reasoning) {
        phase = "reasoning";
    } else if (status.phase == ResponsePhase::answering) {
        phase = "responding";
    }
    std::string status_text = status.active
        ? "[" + status.agent_name + " " + phase + "] "
        : "[Idle] ";
    if (status.active) {
        status_text += " | type /stop or press Esc/Ctrl-C";
    }
    if (!notice.empty()) {
        status_text += " | ";
        status_text += notice;
    }
    attron(A_REVERSE);
    write_status(status_text, status_y, columns);
    attroff(A_REVERSE);

    mvhline(input_y, 0, ACS_HLINE, columns);
    mvhline(input_y + input_height - 1, 0, ACS_HLINE, columns);
    mvvline(input_y, 0, ACS_VLINE, input_height);
    mvvline(input_y, columns - 1, ACS_VLINE, input_height);
    mvaddch(input_y, 0, ACS_ULCORNER);
    mvaddch(input_y, columns - 1, ACS_URCORNER);
    mvaddch(input_y + input_height - 1, 0, ACS_LLCORNER);
    mvaddch(input_y + input_height - 1, columns - 1, ACS_LRCORNER);

    wnoutrefresh(stdscr);
    render_transcript(
        transcript.snapshot(), status, output_height, columns, show_addressing);
    render_input(editor, input_y, input_height, columns);
    doupdate();
}

void Tui::scroll_up() {
    transcript_viewport_.scroll_up();
}

void Tui::scroll_down() {
    transcript_viewport_.scroll_down();
}

void Tui::resize() {
    terminal_.resize();
}

void Tui::replace_pad(WINDOW*& pad, int rows, int columns) {
    if (pad) {
        delwin(pad);
    }
    pad = newpad(std::max(1, rows), std::max(1, columns));
    if (!pad) {
        throw std::runtime_error("Failed to create curses pad");
    }
}

void Tui::ensure_transcript_capacity(int required_rows) {
    if (required_rows <= transcript_capacity_) {
        return;
    }

    const int capacity = std::max(required_rows, std::max(64, transcript_capacity_ * 2));
    if (wresize(transcript_pad_, capacity, transcript_columns_) == ERR) {
        throw std::runtime_error("Failed to grow transcript pad");
    }
    transcript_capacity_ = capacity;
}

void Tui::ensure_input_pad(int required_rows, int columns) {
    if (!input_pad_) {
        const int capacity = std::max(required_rows, 16);
        input_pad_ = newpad(capacity, columns);
        if (!input_pad_) {
            throw std::runtime_error("Failed to create curses input pad");
        }
        input_capacity_ = capacity;
        input_columns_ = columns;
        return;
    }

    if (required_rows <= input_capacity_ && columns == input_columns_) {
        return;
    }

    int resized_capacity = input_capacity_;
    if (required_rows > input_capacity_) {
        resized_capacity = std::max(required_rows, input_capacity_ * 2);
    }
    if (wresize(input_pad_, resized_capacity, columns) == ERR) {
        throw std::runtime_error("Failed to resize curses input pad");
    }
    input_capacity_ = resized_capacity;
    input_columns_ = columns;
}

void Tui::write_transcript_entry(const TranscriptEntry& entry, bool show_addressing) {
    CursesTranscriptSurface surface(transcript_pad_);
    cha::write_transcript_entry(surface, entry, show_addressing);
    getyx(transcript_pad_, rendered_last_content_y_, rendered_last_content_x_);
    waddstr(transcript_pad_, "\n\n");
    wattrset(transcript_pad_, A_NORMAL);
}

void Tui::write_active_response(
    const GenerationStatus& status,
    std::string_view answer_text) {
    CursesTranscriptSurface surface(transcript_pad_);
    cha::write_active_response(
        surface, status.agent_name, status.reasoning_text, answer_text);
    getyx(transcript_pad_, rendered_last_content_y_, rendered_last_content_x_);
    waddstr(transcript_pad_, "\n\n");
    wattrset(transcript_pad_, A_NORMAL);
}

void Tui::rebuild_transcript(
    const TranscriptSnapshot& snapshot,
    const GenerationStatus& status,
    int output_height,
    int columns,
    bool show_addressing) {
    int estimated_rows = output_height + 4;
    const bool decorate_active_answer =
        active_answer_has_reasoning(snapshot, status);
    for (std::size_t index = 0; index < snapshot.entries.size(); ++index) {
        const TranscriptEntry& entry = snapshot.entries[index];
        const bool active_answer =
            decorate_active_answer && index + 1 == snapshot.entries.size();
        const std::string rendered_entry = active_answer
            ? active_response_layout_text(status, entry.text)
            : transcript_entry_label(entry, show_addressing)
                + entry.text + "\n\n";
        estimated_rows += layout_rows(rendered_entry, columns);
    }
    if (status.active
        && status.phase == ResponsePhase::reasoning
        && !status.reasoning_text.empty()) {
        estimated_rows +=
            layout_rows(active_response_layout_text(status, {}), columns);
    }
    replace_pad(transcript_pad_, estimated_rows, columns);
    transcript_capacity_ = estimated_rows;
    transcript_columns_ = columns;

    for (std::size_t index = 0; index < snapshot.entries.size(); ++index) {
        const TranscriptEntry& entry = snapshot.entries[index];
        if (decorate_active_answer && index + 1 == snapshot.entries.size()) {
            write_active_response(status, entry.text);
        } else {
            write_transcript_entry(entry, show_addressing);
        }
    }
    if (status.active
        && status.phase == ResponsePhase::reasoning
        && !status.reasoning_text.empty()) {
        write_active_response(status, {});
    }
}

void Tui::render_transcript(
    const TranscriptSnapshot& snapshot,
    const GenerationStatus& status,
    int output_height,
    int columns,
    bool show_addressing) {
    const TranscriptRenderPlan plan = status == rendered_generation_
        ? transcript_planner_.plan(snapshot, columns)
        : TranscriptRenderPlan{.action = TranscriptRenderAction::rebuild};
    const auto& entries = snapshot.entries;
    if (plan.action == TranscriptRenderAction::rebuild) {
        rebuild_transcript(
            snapshot, status, output_height, columns, show_addressing);
    } else if (plan.action == TranscriptRenderAction::append) {
        const int tail_y = plan.resumes_last_message ? rendered_last_content_y_ : 0;
        const int tail_x = plan.resumes_last_message ? rendered_last_content_x_ : 0;
        std::string rendered_tail = plan.last_message_suffix;
        if (plan.resumes_last_message) {
            rendered_tail += "\n\n";
        }
        for (std::size_t index = plan.first_new_message; index < entries.size(); ++index) {
            rendered_tail += transcript_entry_label(entries[index], show_addressing);
            rendered_tail += entries[index].text;
            rendered_tail += "\n\n";
        }

        const int tail_rows = layout_rows(rendered_tail, columns, tail_x);
        ensure_transcript_capacity(std::max(output_height + 4, tail_y + tail_rows + 4));
        wmove(transcript_pad_, tail_y, tail_x);
        wclrtobot(transcript_pad_);

        if (plan.resumes_last_message) {
            CursesTranscriptSurface surface(transcript_pad_);
            write_transcript_suffix(surface, plan.last_message_suffix);
            getyx(transcript_pad_, rendered_last_content_y_, rendered_last_content_x_);
            waddstr(transcript_pad_, "\n\n");
        }
        for (std::size_t index = plan.first_new_message; index < entries.size(); ++index) {
            write_transcript_entry(entries[index], show_addressing);
        }
    } else {
        ensure_transcript_capacity(output_height + 4);
    }
    transcript_planner_.commit(snapshot, columns);
    rendered_generation_ = status;

    int transcript_lines = 0;
    int cursor_x = 0;
    getyx(transcript_pad_, transcript_lines, cursor_x);
    ++transcript_lines;
    transcript_viewport_.update(transcript_lines, output_height);

    pnoutrefresh(transcript_pad_, transcript_viewport_.top(), 0, 0, 0, output_height - 1, columns - 1);
}

void Tui::render_input(const InputEditor& editor, int input_y, int height, int columns) {
    const int inner_height = height - 2;
    const int inner_width = columns - 2;
    const int estimated_rows = layout_rows(editor.text(), inner_width, 2) + inner_height + 4;
    ensure_input_pad(estimated_rows, inner_width);
    CursesTranscriptSurface surface(input_pad_);
    initialize_transcript_surface(surface);
    werase(input_pad_);
    wmove(input_pad_, 0, 0);

    waddwstr(input_pad_, L"> ");
    const std::wstring prefix = editor.text().substr(0, editor.cursor());
    const std::wstring suffix = editor.text().substr(editor.cursor());
    waddwstr(input_pad_, prefix.c_str());
    int cursor_y = 0;
    int cursor_x = 0;
    getyx(input_pad_, cursor_y, cursor_x);
    waddwstr(input_pad_, suffix.c_str());

    const int input_view_top = std::max(0, cursor_y - inner_height + 1);
    pnoutrefresh(
        input_pad_,
        input_view_top,
        0,
        input_y + 1,
        1,
        input_y + height - 2,
        columns - 2
    );
    move(input_y + 1 + cursor_y - input_view_top, 1 + cursor_x);
}

} // namespace cha
