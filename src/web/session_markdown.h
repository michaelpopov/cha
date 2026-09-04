#pragma once

#include "chat/transcript.h"

#include <span>
#include <string>
#include <string_view>

namespace cha::web {

// A compact transcript export: the filename supplies the visible title, the
// first known entry time appears once, and each message starts with a speaker
// badge. Blank lines within a message become Markdown hard line breaks.
std::string session_markdown(
    std::string_view label,
    std::span<const TranscriptEntry> entries);

} // namespace cha::web
