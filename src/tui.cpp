#include "tui.h"

#include "text_layout.h"

#include <algorithm>
#include <clocale>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {
namespace {

constexpr int input_height = 5;
constexpr int status_height = 1;

std::string speaker_label(Speaker speaker) {
    switch (speaker) {
    case Speaker::user:
        return "You: ";
    case Speaker::assistant:
        return "Assistant: ";
    case Speaker::system:
        return "System: ";
    }
    return {};
}

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

Tui::Tui(std::string model) : model_(std::move(model)) {
    std::setlocale(LC_ALL, "");
    initscr();
    raw();
    noecho();
    keypad(stdscr, true);
    set_escdelay(25);
    nodelay(stdscr, true);
    curs_set(1);
}

Tui::~Tui() {
    if (transcript_pad_) {
        delwin(transcript_pad_);
    }
    if (input_pad_) {
        delwin(input_pad_);
    }
    endwin();
}

int Tui::read_key(wint_t& key) {
    return wget_wch(stdscr, &key);
}

void Tui::render(const Transcript& transcript, const InputEditor& editor, bool generating, std::string_view notice) {
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
    last_output_height_ = output_height;

    erase();

    std::string status = generating ? "[Generating] " : "[Idle] ";
    status += model_;
    if (generating) {
        status += " | type .stop or press Esc/Ctrl-C";
    }
    if (!notice.empty()) {
        status += " | ";
        status += notice;
    }
    attron(A_REVERSE);
    write_status(status, status_y, columns);
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
    render_transcript(transcript, output_height, columns);
    render_input(editor, input_y, input_height, columns);
    doupdate();
}

void Tui::set_model(std::string model) {
    model_ = std::move(model);
}

void Tui::scroll_up() {
    follow_output_ = false;
    view_top_ = std::max(0, view_top_ - std::max(1, last_output_height_ / 2));
}

void Tui::scroll_down() {
    const int bottom = std::max(0, transcript_lines_ - last_output_height_);
    view_top_ = std::min(bottom, view_top_ + std::max(1, last_output_height_ / 2));
    follow_output_ = view_top_ == bottom;
}

void Tui::resize() {
    endwin();
    refresh();
    clear();
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

void Tui::write_transcript_entry(const TranscriptEntry& entry) {
    wattron(transcript_pad_, A_BOLD);
    const std::string label = speaker_label(entry.speaker);
    waddstr(transcript_pad_, label.c_str());
    wattroff(transcript_pad_, A_BOLD);
    waddstr(transcript_pad_, entry.text.c_str());
    getyx(transcript_pad_, rendered_last_content_y_, rendered_last_content_x_);
    waddstr(transcript_pad_, "\n\n");
}

void Tui::rebuild_transcript(const Transcript& transcript, int output_height, int columns) {
    int estimated_rows = output_height + 4;
    for (const TranscriptEntry& entry : transcript.entries()) {
        const std::string rendered_entry = speaker_label(entry.speaker) + entry.text + "\n\n";
        estimated_rows += text_layout::rows(rendered_entry, columns);
    }
    replace_pad(transcript_pad_, estimated_rows, columns);
    transcript_capacity_ = estimated_rows;
    transcript_columns_ = columns;

    for (const TranscriptEntry& entry : transcript.entries()) {
        write_transcript_entry(entry);
    }

    rendered_revision_ = transcript.revision();
    rendered_entry_count_ = transcript.entries().size();
    rendered_last_text_size_ = transcript.entries().empty() ? 0 : transcript.entries().back().text.size();
}

void Tui::render_transcript(const Transcript& transcript, int output_height, int columns) {
    const auto& entries = transcript.entries();
    if (!transcript_pad_ || transcript_columns_ != columns || entries.size() < rendered_entry_count_) {
        rebuild_transcript(transcript, output_height, columns);
    } else if (rendered_revision_ != transcript.revision()) {
        int tail_y = 0;
        int tail_x = 0;
        std::size_t first_new_entry = 0;
        std::string previous_entry_suffix;

        if (rendered_entry_count_ > 0) {
            const TranscriptEntry& previous_last = entries[rendered_entry_count_ - 1];
            if (previous_last.text.size() < rendered_last_text_size_) {
                rebuild_transcript(transcript, output_height, columns);
            } else {
                tail_y = rendered_last_content_y_;
                tail_x = rendered_last_content_x_;
                previous_entry_suffix = previous_last.text.substr(rendered_last_text_size_);
                first_new_entry = rendered_entry_count_;
            }
        }

        if (rendered_revision_ != transcript.revision()) {
            std::string rendered_tail = previous_entry_suffix;
            if (rendered_entry_count_ > 0) {
                rendered_tail += "\n\n";
            }
            for (std::size_t index = first_new_entry; index < entries.size(); ++index) {
                rendered_tail += speaker_label(entries[index].speaker);
                rendered_tail += entries[index].text;
                rendered_tail += "\n\n";
            }

            const int tail_rows = text_layout::rows(rendered_tail, columns, tail_x);
            ensure_transcript_capacity(std::max(output_height + 4, tail_y + tail_rows + 4));
            wmove(transcript_pad_, tail_y, tail_x);
            wclrtobot(transcript_pad_);

            if (rendered_entry_count_ > 0) {
                waddstr(transcript_pad_, previous_entry_suffix.c_str());
                getyx(transcript_pad_, rendered_last_content_y_, rendered_last_content_x_);
                waddstr(transcript_pad_, "\n\n");
            }
            for (std::size_t index = first_new_entry; index < entries.size(); ++index) {
                write_transcript_entry(entries[index]);
            }

            rendered_revision_ = transcript.revision();
            rendered_entry_count_ = entries.size();
            rendered_last_text_size_ = entries.empty() ? 0 : entries.back().text.size();
        }
    } else {
        ensure_transcript_capacity(output_height + 4);
    }

    int cursor_x = 0;
    getyx(transcript_pad_, transcript_lines_, cursor_x);
    ++transcript_lines_;
    const int bottom = std::max(0, transcript_lines_ - output_height);
    if (follow_output_) {
        view_top_ = bottom;
    } else {
        view_top_ = std::min(view_top_, bottom);
    }

    pnoutrefresh(transcript_pad_, view_top_, 0, 0, 0, output_height - 1, columns - 1);
}

void Tui::render_input(const InputEditor& editor, int input_y, int height, int columns) {
    const int inner_height = height - 2;
    const int inner_width = columns - 2;
    const int estimated_rows = text_layout::rows(editor.text(), inner_width, 2) + inner_height + 4;
    replace_pad(input_pad_, estimated_rows, inner_width);

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
