#include "util/public_name.h"

#include "util/utf8_path.h"

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
    if ((first & 0xe0) == 0xc0) { continuation_count = 1; result = first & 0x1f; }
    else if ((first & 0xf0) == 0xe0) { continuation_count = 2; result = first & 0x0f; }
    else if ((first & 0xf8) == 0xf0) { continuation_count = 3; result = first & 0x07; }
    else throw std::runtime_error("invalid UTF-8");
    if (continuation_count > text.size() - offset) throw std::runtime_error("invalid UTF-8");
    for (std::size_t index{}; index < continuation_count; ++index) {
        const unsigned char continuation = byte(offset++);
        if ((continuation & 0xc0) != 0x80) throw std::runtime_error("invalid UTF-8");
        result = (result << 6) | (continuation & 0x3f);
    }
    if ((continuation_count == 1 && result < 0x80)
        || (continuation_count == 2 && result < 0x800)
        || (continuation_count == 3 && result < 0x10000)
        || result > 0x10ffff || (result >= 0xd800 && result <= 0xdfff)) {
        throw std::runtime_error("invalid UTF-8");
    }
    return result;
}

bool whitespace(char32_t value) {
    return (value >= 0x0009 && value <= 0x000d) || value == 0x0020
        || value == 0x0085 || value == 0x00a0 || value == 0x1680
        || (value >= 0x2000 && value <= 0x200a) || value == 0x2028
        || value == 0x2029 || value == 0x202f || value == 0x205f || value == 0x3000;
}

bool control(char32_t value) { return value <= 0x001f || (value >= 0x007f && value <= 0x009f); }

void validate_text(std::string_view value, std::string_view entity, const std::filesystem::path& source) {
    if (value.empty()) throw std::runtime_error(std::string(entity) + " in '" + utf8_path(source) + "' must not be empty");
    char32_t first{};
    char32_t last{};
    bool is_first = true;
    try {
        for (std::size_t offset{}; offset < value.size();) {
            const char32_t point = next_code_point(value, offset);
            if (control(point) || point == 0x2028 || point == 0x2029) {
                throw std::runtime_error("contains a control character or line break");
            }
            if (is_first) {
                first = point;
                is_first = false;
            }
            last = point;
        }
    } catch (const std::runtime_error& error) {
        throw std::runtime_error(std::string(entity) + " in '" + utf8_path(source) + "' " + error.what());
    }
    if (whitespace(first) || whitespace(last)) {
        throw std::runtime_error(std::string(entity) + " in '" + utf8_path(source) + "' cannot start or end with whitespace");
    }
}

} // namespace

void validate_public_name(std::string_view name, std::string_view entity, const std::filesystem::path& source, bool participant_name) {
    validate_text(name, entity, source);
    if (participant_name && (name.front() == '@' || name.front() == '/')) {
        throw std::runtime_error(std::string(entity) + " in '" + utf8_path(source) + "' cannot start with '@' or '/'");
    }
}

void validate_description(std::string_view description, std::string_view entity, const std::filesystem::path& source) {
    validate_text(description, std::string(entity) + " description", source);
}

} // namespace cha
