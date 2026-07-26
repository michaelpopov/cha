#include "ui/console/console_writer.h"

#include <ostream>

namespace cha {
namespace {

bool is_c1_sequence(std::string_view text, std::size_t index) {
    return index + 1 < text.size()
        && static_cast<unsigned char>(text[index]) == 0xc2
        && static_cast<unsigned char>(text[index + 1]) >= 0x80
        && static_cast<unsigned char>(text[index + 1]) <= 0x9f;
}

} // namespace

ConsoleSurface::ConsoleSurface(std::ostream& output, bool color)
    : output_(output),
      color_(color) {
}

void ConsoleSurface::attributes(TranscriptAttributes value) {
    if (!color_) {
        return;
    }
    switch (value) {
    case TranscriptAttributes::normal:
        output_ << "\x1b[0m";
        break;
    case TranscriptAttributes::bold:
        output_ << "\x1b[1m";
        break;
    case TranscriptAttributes::dim:
        output_ << "\x1b[2m";
        break;
    case TranscriptAttributes::bold_dim:
        output_ << "\x1b[1;2m";
        break;
    }
}

void ConsoleSurface::write(std::string_view text) {
    output_ << sanitize_console_text(text);
}

std::string sanitize_console_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte =
            static_cast<unsigned char>(text[index]);
        if (is_c1_sequence(text, index)) {
            result += "[C1]";
            ++index;
        } else if (byte == '\n' || byte == '\t') {
            result.push_back(static_cast<char>(byte));
        } else if (byte == '\r') {
            continue;
        } else if (byte < 0x20) {
            result.push_back('^');
            result.push_back(static_cast<char>(byte + 0x40));
        } else if (byte == 0x7f) {
            result += "^?";
        } else {
            result.push_back(static_cast<char>(byte));
        }
    }
    return result;
}

} // namespace cha
