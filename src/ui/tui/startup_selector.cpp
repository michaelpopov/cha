#include "ui/tui/startup_selector.h"

#include "ui/tui/input_editor.h"
#include "ui/tui/terminal.h"

#include <curses.h>

#include <algorithm>
#include <cassert>
#include <cwchar>
#include <cwctype>
#include <string>
#include <string_view>

namespace cha {
namespace {

std::wstring_view visible_suffix(std::wstring_view text, int available_cells) {
    int used_cells = 0;
    std::size_t start = text.size();
    while (start > 0) {
        const int width = std::max(0, ::wcwidth(text[start - 1]));
        if (used_cells + width > available_cells) {
            break;
        }
        used_cells += width;
        --start;
    }
    return text.substr(start);
}

} // namespace

StartupSelector::StartupSelector(Terminal& terminal) : terminal_(terminal) {
    terminal_.configure_selector();
}

std::optional<User> StartupSelector::select_user(const UserRoster& users) {
    std::vector<std::string> options;
    options.reserve(users.size());
    for (const User& user : users) {
        options.push_back(user.display_name);
    }
    const std::optional<std::size_t> selection =
        select("Select a user", options);
    assert(!selection || *selection < users.size());
    return selection ? std::optional<User>(users[*selection]) : std::nullopt;
}

std::optional<std::string> StartupSelector::select_forum(const std::vector<Forum>& forums) {
    std::vector<std::string> options;
    options.reserve(forums.size());
    for (const Forum& forum : forums) {
        options.push_back(forum.display_name);
    }
    const auto selected = select("Select a forum", options);
    return selected ? std::optional<std::string>(forums[*selected].name) : std::nullopt;
}

std::optional<SessionSummary> StartupSelector::select_session(
    const std::vector<SessionSummary>& sessions,
    std::string_view error) {
    std::vector<std::string> options{"+  New session"};
    for (const SessionSummary& session : sessions) {
        options.push_back(session.label);
    }
    const auto selected = select("Select a session", options, 0, error);
    if (!selected) {
        return std::nullopt;
    }
    return *selected == 0
        ? std::optional<SessionSummary>(SessionSummary{})
        : std::optional<SessionSummary>(sessions[*selected - 1]);
}

std::optional<std::string> StartupSelector::prompt_session_name() {
    InputEditor editor;
    while (true) {
        erase();
        int rows = 0;
        int columns = 0;
        getmaxyx(stdscr, rows, columns);
        mvaddnstr(1, 2, "Name the new session (optional)", std::max(0, columns - 4));
        mvaddnstr(3, 2, "> ", std::max(0, columns - 4));
        const std::wstring_view displayed_name = visible_suffix(editor.text(), std::max(0, columns - 6));
        move(3, 4);
        if (!displayed_name.empty()) {
            waddnwstr(stdscr, displayed_name.data(), static_cast<int>(displayed_name.size()));
        }
        int cursor_y = 0;
        int cursor_x = 0;
        getyx(stdscr, cursor_y, cursor_x);
        mvaddnstr(rows - 2, 2, "Enter: continue  Esc: cancel", std::max(0, columns - 4));
        move(cursor_y, cursor_x);
        refresh();

        wint_t key = 0;
        const int key_result = wget_wch(stdscr, &key);
        if (key_result == ERR) {
            continue;
        }
        if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            return editor.value();
        }
        if (key == 27) {
            return std::nullopt;
        }
        if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
            editor.backspace();
        } else if (key_result == OK && std::iswprint(key) != 0) {
            editor.insert(static_cast<wchar_t>(key));
        } else if (key == KEY_RESIZE) {
            terminal_.resize();
        }
    }
}

std::optional<std::size_t> StartupSelector::select(
    const std::string& title,
    const std::vector<std::string>& options,
    std::optional<std::size_t> emphasized_option,
    std::string_view error) {
    std::size_t current = 0;
    while (true) {
        erase();
        int rows = 0;
        int columns = 0;
        getmaxyx(stdscr, rows, columns);
        mvaddnstr(1, 2, title.c_str(), std::max(0, columns - 4));
        if (!error.empty()) {
            attron(A_BOLD);
            mvaddnstr(2, 2, error.data(), std::min(static_cast<int>(error.size()), std::max(0, columns - 4)));
            attroff(A_BOLD);
        }
        mvaddnstr(rows - 2, 2, "Arrow keys: move  Enter: select  Esc/q: cancel", std::max(0, columns - 4));

        const int first_row = 3;
        const int visible = std::max(1, rows - first_row - 3);
        const std::size_t top = current >= static_cast<std::size_t>(visible)
            ? current - static_cast<std::size_t>(visible) + 1 : 0;
        for (int row = 0; row < visible && top + static_cast<std::size_t>(row) < options.size(); ++row) {
            const std::size_t index = top + static_cast<std::size_t>(row);
            if (emphasized_option && index == *emphasized_option) {
                attron(A_BOLD);
            }
            if (index == current) {
                attron(A_REVERSE);
            }
            mvprintw(first_row + row, 2, "%s", options[index].c_str());
            if (index == current) {
                attroff(A_REVERSE);
            }
            if (emphasized_option && index == *emphasized_option) {
                attroff(A_BOLD);
            }
        }
        refresh();

        const int key = getch();
        if (key == KEY_UP) {
            current = current == 0 ? options.size() - 1 : current - 1;
        } else if (key == KEY_DOWN) {
            current = (current + 1) % options.size();
        } else if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            return current;
        } else if (key == 27 || key == 'q' || key == 'Q') {
            return std::nullopt;
        } else if (key == KEY_RESIZE) {
            terminal_.resize();
        }
    }
}

} // namespace cha
