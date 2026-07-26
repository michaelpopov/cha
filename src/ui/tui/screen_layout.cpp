#include "ui/tui/screen_layout.h"

#include <algorithm>
#include <cwchar>

namespace cha {
namespace {

void add_cells(int cells, int columns, int& rows, int& column) {
    for (int cell = 0; cell < cells; ++cell) {
        ++column;
        if (column >= columns) {
            ++rows;
            column = 0;
        }
    }
}

} // namespace

void TranscriptViewport::update(int transcript_lines, int output_height) {
    transcript_lines_ = std::max(0, transcript_lines);
    output_height_ = std::max(1, output_height);
    if (follows_output_) {
        top_ = bottom();
    } else {
        top_ = std::min(top_, bottom());
    }
}

void TranscriptViewport::scroll_up() {
    follows_output_ = false;
    top_ = std::max(0, top_ - std::max(1, output_height_ / 2));
}

void TranscriptViewport::scroll_down() {
    top_ = std::min(bottom(), top_ + std::max(1, output_height_ / 2));
    follows_output_ = top_ == bottom();
}

int TranscriptViewport::top() const {
    return top_;
}

bool TranscriptViewport::follows_output() const {
    return follows_output_;
}

int TranscriptViewport::bottom() const {
    return std::max(0, transcript_lines_ - output_height_);
}

int layout_rows(std::string_view text, int columns, int initial_cells) {
    columns = std::max(1, columns);
    int result = 1;
    int column = 0;
    add_cells(initial_cells, columns, result, column);

    for (const unsigned char character : text) {
        if (character == '\n') {
            ++result;
            column = 0;
        } else if (character == '\r') {
            column = 0;
        } else if (character == '\t') {
            add_cells(8 - (column % 8), columns, result, column);
        } else {
            add_cells(1, columns, result, column);
        }
    }

    return result;
}

int layout_rows(std::wstring_view text, int columns, int initial_cells) {
    columns = std::max(1, columns);
    int result = 1;
    int column = 0;
    add_cells(initial_cells, columns, result, column);

    for (const wchar_t character : text) {
        if (character == L'\n') {
            ++result;
            column = 0;
        } else if (character == L'\r') {
            column = 0;
        } else if (character == L'\t') {
            add_cells(8 - (column % 8), columns, result, column);
        } else {
            add_cells(std::max(0, ::wcwidth(character)), columns, result, column);
        }
    }

    return result;
}

} // namespace cha
