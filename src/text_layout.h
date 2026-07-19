#pragma once

#include <algorithm>
#include <cwchar>
#include <string_view>

namespace cha::text_layout {
namespace detail {

inline void add_cells(int cells, int columns, int& rows, int& column) {
    for (int cell = 0; cell < cells; ++cell) {
        ++column;
        if (column >= columns) {
            ++rows;
            column = 0;
        }
    }
}

} // namespace detail

inline int rows(std::string_view text, int columns, int initial_cells = 0) {
    columns = std::max(1, columns);
    int result = 1;
    int column = 0;
    detail::add_cells(initial_cells, columns, result, column);

    for (const unsigned char character : text) {
        if (character == '\n') {
            ++result;
            column = 0;
        } else if (character == '\r') {
            column = 0;
        } else if (character == '\t') {
            detail::add_cells(8 - (column % 8), columns, result, column);
        } else {
            // Counting UTF-8 bytes as cells is conservative and cannot
            // underestimate the pad height for multibyte characters.
            detail::add_cells(1, columns, result, column);
        }
    }

    return result;
}

inline int rows(std::wstring_view text, int columns, int initial_cells = 0) {
    columns = std::max(1, columns);
    int result = 1;
    int column = 0;
    detail::add_cells(initial_cells, columns, result, column);

    for (const wchar_t character : text) {
        if (character == L'\n') {
            ++result;
            column = 0;
        } else if (character == L'\r') {
            column = 0;
        } else if (character == L'\t') {
            detail::add_cells(8 - (column % 8), columns, result, column);
        } else {
            const int width = ::wcwidth(character);
            detail::add_cells(std::max(0, width), columns, result, column);
        }
    }

    return result;
}

} // namespace cha::text_layout
