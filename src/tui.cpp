#include "tui.h"

#include "terminal.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace cha {
namespace {

constexpr int input_height = 5;
constexpr int status_height = 1;

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
    const Conversation& conversation,
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

    std::string status_text = status.active ? "[" + status.agent_name + " generating] " : "[Idle] ";
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
    render_transcript(conversation.snapshot(), output_height, columns, show_addressing);
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

bool Tui::input_closed() const {
    return !::isatty(STDIN_FILENO);
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

void Tui::write_transcript_entry(const ConversationEntry& entry, bool show_addressing) {
    wattron(transcript_pad_, A_BOLD);
    const std::string label = transcript_entry_label(entry, show_addressing);
    waddstr(transcript_pad_, label.c_str());
    wattroff(transcript_pad_, A_BOLD);
    waddstr(transcript_pad_, entry.text.c_str());
    getyx(transcript_pad_, rendered_last_content_y_, rendered_last_content_x_);
    waddstr(transcript_pad_, "\n\n");
}

void Tui::rebuild_transcript(const ConversationSnapshot& snapshot, int output_height, int columns, bool show_addressing) {
    int estimated_rows = output_height + 4;
    for (const ConversationEntry& entry : snapshot.entries) {
        const std::string rendered_entry = transcript_entry_label(entry, show_addressing) + entry.text + "\n\n";
        estimated_rows += layout_rows(rendered_entry, columns);
    }
    replace_pad(transcript_pad_, estimated_rows, columns);
    transcript_capacity_ = estimated_rows;
    transcript_columns_ = columns;

    for (const ConversationEntry& entry : snapshot.entries) {
        write_transcript_entry(entry, show_addressing);
    }
}

void Tui::render_transcript(const ConversationSnapshot& snapshot, int output_height, int columns, bool show_addressing) {
    const TranscriptRenderPlan plan = transcript_planner_.plan(snapshot, columns);
    const auto& entries = snapshot.entries;
    if (plan.action == TranscriptRenderAction::rebuild) {
        rebuild_transcript(snapshot, output_height, columns, show_addressing);
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
            waddstr(transcript_pad_, plan.last_message_suffix.c_str());
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
