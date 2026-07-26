#pragma once

#include <string_view>

namespace cha {

class TranscriptViewport {
public:
    void update(int transcript_lines, int output_height);
    void scroll_up();
    void scroll_down();

    int top() const;
    bool follows_output() const;

private:
    int bottom() const;

    int transcript_lines_{};
    int output_height_{};
    int top_{};
    bool follows_output_{true};
};

int layout_rows(std::string_view text, int columns, int initial_cells = 0);
int layout_rows(std::wstring_view text, int columns, int initial_cells = 0);

} // namespace cha
