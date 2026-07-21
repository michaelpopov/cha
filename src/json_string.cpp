#include "json_string.h"

#include <stdexcept>

namespace cha {
namespace {

[[noreturn]] void invalid_utf8() {
    throw std::runtime_error("Completion request contains invalid UTF-8");
}

bool continuation(unsigned char byte) {
    return byte >= 0x80 && byte <= 0xbf;
}

void append_escape(std::string& output, unsigned char byte) {
    switch (byte) {
    case '"': output += "\\\""; return;
    case '\\': output += "\\\\"; return;
    case '\b': output += "\\b"; return;
    case '\f': output += "\\f"; return;
    case '\n': output += "\\n"; return;
    case '\r': output += "\\r"; return;
    case '\t': output += "\\t"; return;
    default:
        constexpr char hex[] = "0123456789abcdef";
        output += "\\u00";
        output += hex[(byte >> 4) & 0x0f];
        output += hex[byte & 0x0f];
    }
}

} // namespace

void append_json_string_content(std::string& output, std::string_view text) {
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first <= 0x1f) {
            append_escape(output, first);
            ++index;
            continue;
        }
        if (first == '"' || first == '\\') {
            append_escape(output, first);
            ++index;
            continue;
        }
        if (first <= 0x7f) {
            output += static_cast<char>(first);
            ++index;
            continue;
        }

        std::size_t length = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            length = 2;
            if (index + length > text.size()
                || !continuation(static_cast<unsigned char>(text[index + 1]))) {
                invalid_utf8();
            }
        } else if (first == 0xe0) {
            length = 3;
            if (index + length > text.size()) {
                invalid_utf8();
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            if (second < 0xa0 || second > 0xbf
                || !continuation(static_cast<unsigned char>(text[index + 2]))) {
                invalid_utf8();
            }
        } else if ((first >= 0xe1 && first <= 0xec)
                   || (first >= 0xee && first <= 0xef)) {
            length = 3;
            if (index + length > text.size()
                || !continuation(static_cast<unsigned char>(text[index + 1]))
                || !continuation(static_cast<unsigned char>(text[index + 2]))) {
                invalid_utf8();
            }
        } else if (first == 0xed) {
            length = 3;
            if (index + length > text.size()) {
                invalid_utf8();
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            if (second < 0x80 || second > 0x9f
                || !continuation(static_cast<unsigned char>(text[index + 2]))) {
                invalid_utf8();
            }
        } else if (first == 0xf0) {
            length = 4;
            if (index + length > text.size()) {
                invalid_utf8();
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            if (second < 0x90 || second > 0xbf
                || !continuation(static_cast<unsigned char>(text[index + 2]))
                || !continuation(static_cast<unsigned char>(text[index + 3]))) {
                invalid_utf8();
            }
        } else if (first >= 0xf1 && first <= 0xf3) {
            length = 4;
            if (index + length > text.size()
                || !continuation(static_cast<unsigned char>(text[index + 1]))
                || !continuation(static_cast<unsigned char>(text[index + 2]))
                || !continuation(static_cast<unsigned char>(text[index + 3]))) {
                invalid_utf8();
            }
        } else if (first == 0xf4) {
            length = 4;
            if (index + length > text.size()) {
                invalid_utf8();
            }
            const unsigned char second = static_cast<unsigned char>(text[index + 1]);
            if (second < 0x80 || second > 0x8f
                || !continuation(static_cast<unsigned char>(text[index + 2]))
                || !continuation(static_cast<unsigned char>(text[index + 3]))) {
                invalid_utf8();
            }
        } else {
            invalid_utf8();
        }

        output.append(text.data() + index, length);
        index += length;
    }
}

} // namespace cha
