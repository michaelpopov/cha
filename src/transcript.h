#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

enum class Speaker {
    user,
    assistant,
    system,
};

struct TranscriptEntry {
    Speaker speaker;
    std::string text;
};

class Transcript {
public:
    void add_user(std::string text);
    void begin_assistant();
    void append_assistant(std::string_view text);
    void finish_assistant();
    void add_system(std::string text);

    [[nodiscard]] const std::vector<TranscriptEntry>& entries() const;
    [[nodiscard]] std::size_t revision() const;

private:
    std::vector<TranscriptEntry> entries_;
    bool assistant_open_{};
    std::size_t revision_{};
};

} // namespace cha
