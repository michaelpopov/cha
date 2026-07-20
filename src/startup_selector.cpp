#include "startup_selector.h"

#include <curses.h>

#include <algorithm>
#include <clocale>
#include <string>

namespace cha {

StartupSelector::StartupSelector() {
    std::setlocale(LC_ALL, "");
    initscr();
    raw();
    noecho();
    keypad(stdscr, true);
    curs_set(0);
}

StartupSelector::~StartupSelector() {
    endwin();
}

std::optional<std::string> StartupSelector::select_room(const std::vector<std::string>& rooms) {
    const auto selected = select("Select a room", rooms);
    return selected ? std::optional<std::string>(rooms[*selected]) : std::nullopt;
}

std::optional<Session> StartupSelector::select_session(const std::vector<Session>& sessions) {
    std::vector<std::string> options{"New session"};
    for (const Session& session : sessions) {
        options.push_back(session.label);
    }
    const auto selected = select("Select a session", options);
    if (!selected) {
        return std::nullopt;
    }
    return *selected == 0 ? std::optional<Session>(Session{}) : std::optional<Session>(sessions[*selected - 1]);
}

std::optional<std::string> StartupSelector::prompt_session_name() {
    std::string name;
    while (true) {
        erase();
        int rows = 0;
        int columns = 0;
        getmaxyx(stdscr, rows, columns);
        mvaddnstr(1, 2, "Name the new session (optional)", std::max(0, columns - 4));
        mvaddnstr(3, 2, "> ", std::max(0, columns - 4));
        mvaddnstr(3, 4, name.c_str(), std::max(0, columns - 6));
        mvaddnstr(rows - 2, 2, "Enter: continue  Esc: cancel", std::max(0, columns - 4));
        move(3, std::min(columns - 1, 4 + static_cast<int>(name.size())));
        refresh();

        const int key = getch();
        if (key == '\n' || key == '\r' || key == KEY_ENTER) {
            return name;
        }
        if (key == 27) {
            return std::nullopt;
        }
        if (key == KEY_BACKSPACE || key == 127 || key == '\b') {
            if (!name.empty()) {
                name.pop_back();
            }
        } else if (key >= 32 && key <= 126) {
            name.push_back(static_cast<char>(key));
        } else if (key == KEY_RESIZE) {
            endwin();
            refresh();
            clear();
        }
    }
}

std::optional<std::size_t> StartupSelector::select(const std::string& title, const std::vector<std::string>& options) {
    std::size_t current = 0;
    while (true) {
        erase();
        int rows = 0;
        int columns = 0;
        getmaxyx(stdscr, rows, columns);
        mvaddnstr(1, 2, title.c_str(), std::max(0, columns - 4));
        mvaddnstr(rows - 2, 2, "Arrow keys: move  Enter: select  Esc/q: cancel", std::max(0, columns - 4));

        const int first_row = 3;
        const int visible = std::max(1, rows - first_row - 3);
        const std::size_t top = current >= static_cast<std::size_t>(visible)
            ? current - static_cast<std::size_t>(visible) + 1 : 0;
        for (int row = 0; row < visible && top + static_cast<std::size_t>(row) < options.size(); ++row) {
            const std::size_t index = top + static_cast<std::size_t>(row);
            if (index == current) {
                attron(A_REVERSE);
            }
            mvprintw(first_row + row, 2, "%s", options[index].c_str());
            if (index == current) {
                attroff(A_REVERSE);
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
            endwin();
            refresh();
            clear();
        }
    }
}

} // namespace cha
