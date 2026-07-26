#include "ui/console/console_writer.h"

#include <ostream>

namespace cha {
namespace {

constexpr unsigned char c1_lead = 0xc2;
constexpr std::string_view c1_replacement = "[C1]";

bool is_c1_continuation(unsigned char byte) {
    return byte >= 0x80 && byte <= 0x9f;
}

} // namespace

ConsoleSurface::ConsoleSurface(std::ostream& output, bool color)
    : output_(output),
      color_(color) {
}

void ConsoleSurface::finish() {
    if (held_lead_) {
        output_.put(static_cast<char>(c1_lead));
        held_lead_ = false;
    }
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
    output_ << sanitize_console_chunk(text, held_lead_);
}

std::string sanitize_console_text(std::string_view text) {
    bool held_lead = false;
    std::string result = sanitize_console_chunk(text, held_lead);
    if (held_lead) {
        result.push_back(static_cast<char>(c1_lead));
    }
    return result;
}

std::string sanitize_console_chunk(std::string_view text, bool& held_lead) {
    // An empty write contributes no byte to the stream and therefore cannot
    // decide whether a previously held lead byte begins a C1 control.
    if (text.empty()) {
        return {};
    }

    std::string result;
    result.reserve(text.size() + 1);
    std::size_t index = 0;
    if (held_lead) {
        held_lead = false;
        if (is_c1_continuation(
                static_cast<unsigned char>(text.front()))) {
            result += c1_replacement;
            index = 1;
        } else {
            result.push_back(static_cast<char>(c1_lead));
        }
    }

    for (; index < text.size(); ++index) {
        const unsigned char byte =
            static_cast<unsigned char>(text[index]);
        if (byte == c1_lead) {
            if (index + 1 == text.size()) {
                held_lead = true;
                break;
            }
            if (is_c1_continuation(
                    static_cast<unsigned char>(text[index + 1]))) {
                result += c1_replacement;
                ++index;
                continue;
            }
            result.push_back(static_cast<char>(byte));
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
