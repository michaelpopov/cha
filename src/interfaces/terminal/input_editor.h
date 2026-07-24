#pragma once

#include <cstddef>
#include <string>

namespace cha {

// Maintains editable multiline terminal input and converts it to submitted UTF-8 text.
class InputEditor {
public:
    void insert(wchar_t character);
    void backspace();
    void erase();
    void move_left();
    void move_right();
    void move_home();
    void move_end();
    void move_up();
    void move_down();
    void continue_line();
    void clear();

    bool ends_with_continuation() const;
    const std::wstring& text() const;
    std::size_t cursor() const;
    std::string value() const;

private:
    std::size_t line_start(std::size_t position) const;
    std::size_t line_end(std::size_t position) const;

    std::wstring text_;
    std::size_t cursor_{};
};

} // namespace cha
