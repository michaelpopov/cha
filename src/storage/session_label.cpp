#include "storage/session_label.h"

#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace cha {
namespace {

[[noreturn]] void invalid_utf8(std::string_view field) {
    throw std::invalid_argument(std::string(field) + " must be valid UTF-8");
}

char32_t next_code_point(
    std::string_view text,
    std::size_t& offset,
    std::string_view field) {
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
        invalid_utf8(field);
    }
    if (continuation_count > text.size() - offset) invalid_utf8(field);
    for (std::size_t index{}; index < continuation_count; ++index) {
        const unsigned char continuation = byte(offset++);
        if ((continuation & 0xc0) != 0x80) invalid_utf8(field);
        result = (result << 6) | (continuation & 0x3f);
    }
    if ((continuation_count == 1 && result < 0x80)
        || (continuation_count == 2 && result < 0x800)
        || (continuation_count == 3 && result < 0x10000)
        || result > 0x10ffff
        || (result >= 0xd800 && result <= 0xdfff)) {
        invalid_utf8(field);
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

bool local_time(std::time_t now, std::tm& result) {
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    const std::tm* const local = std::localtime(&now);
    if (local == nullptr) return false;
    result = *local;
    return true;
}

} // namespace

void validate_display_label(std::string_view label, std::string_view field) {
    const auto reject = [field](std::string_view tail) {
        throw std::invalid_argument(std::string(field) + std::string(tail));
    };
    if (label.empty()) reject(" must not be empty");

    char32_t first{};
    char32_t last{};
    std::size_t count{};
    for (std::size_t offset{}; offset < label.size();) {
        const char32_t point = next_code_point(label, offset, field);
        if (control(point)) {
            reject(" cannot contain a control character or line break");
        }
        if (count == 0) first = point;
        last = point;
        ++count;
        if (count > 200) reject(" cannot exceed 200 Unicode characters");
    }
    if (whitespace(first) || whitespace(last)) {
        reject(" cannot start or end with whitespace");
    }
}

void validate_session_label(std::string_view label) {
    validate_display_label(label, "Session label");
}

std::string display_label_prefix(std::string_view label, std::size_t count) {
    std::size_t offset{};
    std::size_t end{};
    for (std::size_t points{}; offset < label.size() && points < count; ++points) {
        if (!whitespace(next_code_point(label, offset, "Label"))) end = offset;
    }
    return std::string(label.substr(0, end));
}

std::string session_timestamp_name(std::time_t now) {
    std::tm local{};
    if (!local_time(now, local)) {
        throw std::runtime_error("Failed to read local time for session name");
    }
    std::ostringstream result;
    result << std::put_time(&local, "%Y-%m-%d-%H-%M-%S");
    return result.str();
}

} // namespace cha
