#include "interfaces/terminal/input_editor.h"

#include <algorithm>
#include <cstdint>

namespace cha {
namespace {

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

} // namespace

void InputEditor::insert(wchar_t character) {
    text_.insert(text_.begin() + static_cast<std::ptrdiff_t>(cursor_), character);
    ++cursor_;
}

void InputEditor::backspace() {
    if (cursor_ == 0) {
        return;
    }
    text_.erase(cursor_ - 1, 1);
    --cursor_;
}

void InputEditor::erase() {
    if (cursor_ < text_.size()) {
        text_.erase(cursor_, 1);
    }
}

void InputEditor::move_left() {
    if (cursor_ > 0) {
        --cursor_;
    }
}

void InputEditor::move_right() {
    if (cursor_ < text_.size()) {
        ++cursor_;
    }
}

void InputEditor::move_home() {
    cursor_ = line_start(cursor_);
}

void InputEditor::move_end() {
    cursor_ = line_end(cursor_);
}

void InputEditor::move_up() {
    const std::size_t current_start = line_start(cursor_);
    if (current_start == 0) {
        return;
    }

    const std::size_t column = cursor_ - current_start;
    const std::size_t previous_end = current_start - 1;
    const std::size_t previous_start = line_start(previous_end);
    cursor_ = previous_start + std::min(column, previous_end - previous_start);
}

void InputEditor::move_down() {
    const std::size_t current_start = line_start(cursor_);
    const std::size_t current_end = line_end(cursor_);
    if (current_end == text_.size()) {
        return;
    }

    const std::size_t column = cursor_ - current_start;
    const std::size_t next_start = current_end + 1;
    const std::size_t next_end = line_end(next_start);
    cursor_ = next_start + std::min(column, next_end - next_start);
}

void InputEditor::continue_line() {
    if (!ends_with_continuation()) {
        return;
    }

    text_.pop_back();
    cursor_ = text_.size();
    insert(L'\n');
}

void InputEditor::clear() {
    text_.clear();
    cursor_ = 0;
}

bool InputEditor::ends_with_continuation() const {
    return cursor_ == text_.size() && !text_.empty() && text_.back() == L'\\';
}

const std::wstring& InputEditor::text() const {
    return text_;
}

std::size_t InputEditor::cursor() const {
    return cursor_;
}

std::string InputEditor::value() const {
    std::string output;
    for (const wchar_t character : text_) {
        if (character != L'\n') {
            append_utf8(output, static_cast<std::uint32_t>(character));
        }
    }
    return output;
}

std::size_t InputEditor::line_start(std::size_t position) const {
    if (position == 0) {
        return 0;
    }
    const std::size_t newline = text_.rfind(L'\n', position - 1);
    return newline == std::wstring::npos ? 0 : newline + 1;
}

std::size_t InputEditor::line_end(std::size_t position) const {
    const std::size_t newline = text_.find(L'\n', position);
    return newline == std::wstring::npos ? text_.size() : newline;
}

} // namespace cha
