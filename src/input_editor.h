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

    [[nodiscard]] bool ends_with_continuation() const;
    [[nodiscard]] const std::wstring& text() const;
    [[nodiscard]] std::size_t cursor() const;
    [[nodiscard]] std::string value() const;

private:
    [[nodiscard]] std::size_t line_start(std::size_t position) const;
    [[nodiscard]] std::size_t line_end(std::size_t position) const;

    std::wstring text_;
    std::size_t cursor_{};
};

} // namespace cha
