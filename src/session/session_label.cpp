#include "session/session_label.h"

#include <stdexcept>

namespace cha {
namespace {

char32_t next_code_point(std::string_view text, std::size_t& offset) {
    const auto byte = [&text](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    const unsigned char first = byte(offset++);
    if (first < 0x80) return first;

    std::size_t continuation_count{};
    char32_t result{};
    if ((first & 0xe0) == 0xc0) {
        continuation_count = 1;
        result = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        continuation_count = 2;
        result = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        continuation_count = 3;
        result = first & 0x07;
    } else {
        throw std::invalid_argument("Session label must be valid UTF-8");
    }
    if (continuation_count > text.size() - offset) {
        throw std::invalid_argument("Session label must be valid UTF-8");
    }
    for (std::size_t index{}; index < continuation_count; ++index) {
        const unsigned char continuation = byte(offset++);
        if ((continuation & 0xc0) != 0x80) {
            throw std::invalid_argument("Session label must be valid UTF-8");
        }
        result = (result << 6) | (continuation & 0x3f);
    }
    if ((continuation_count == 1 && result < 0x80)
        || (continuation_count == 2 && result < 0x800)
        || (continuation_count == 3 && result < 0x10000)
        || result > 0x10ffff
        || (result >= 0xd800 && result <= 0xdfff)) {
        throw std::invalid_argument("Session label must be valid UTF-8");
    }
    return result;
}

bool whitespace(char32_t value) {
    return (value >= 0x0009 && value <= 0x000d) || value == 0x0020
        || value == 0x0085 || value == 0x00a0 || value == 0x1680
        || (value >= 0x2000 && value <= 0x200a) || value == 0x2028
        || value == 0x2029 || value == 0x202f || value == 0x205f
        || value == 0x3000;
}

bool control(char32_t value) {
    return value <= 0x001f || (value >= 0x007f && value <= 0x009f)
        || value == 0x2028 || value == 0x2029;
}

} // namespace

void validate_session_label(std::string_view label) {
    if (label.empty()) {
        throw std::invalid_argument("Session label must not be empty");
    }

    char32_t first{};
    char32_t last{};
    std::size_t count{};
    for (std::size_t offset{}; offset < label.size();) {
        const char32_t point = next_code_point(label, offset);
        if (control(point)) {
            throw std::invalid_argument(
                "Session label cannot contain a control character or line break");
        }
        if (count == 0) first = point;
        last = point;
        ++count;
        if (count > 200) {
            throw std::invalid_argument(
                "Session label cannot exceed 200 Unicode characters");
        }
    }
    if (whitespace(first) || whitespace(last)) {
        throw std::invalid_argument(
            "Session label cannot start or end with whitespace");
    }
}

} // namespace cha
