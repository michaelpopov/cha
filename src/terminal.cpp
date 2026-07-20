#include "terminal.h"

#include <curses.h>

#include <clocale>

namespace cha {

Terminal::Terminal() {
    std::setlocale(LC_ALL, "");
    initscr();
    raw();
    noecho();
    keypad(stdscr, true);
    set_escdelay(25);
}

Terminal::~Terminal() {
    endwin();
}

void Terminal::configure_selector() {
    nodelay(stdscr, false);
    curs_set(0);
}

void Terminal::configure_chat() {
    nodelay(stdscr, true);
    curs_set(1);
}

void Terminal::resize() {
    endwin();
    refresh();
    clear();
}

} // namespace cha
