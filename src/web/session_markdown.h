#pragma once

#include "chat/transcript.h"

#include <span>
#include <string>
#include <string_view>

namespace cha::web {

// A portable transcript export: the session and each speaker become headings,
// while entry text stays verbatim so any Markdown in the conversation survives.
std::string session_markdown(
    std::string_view label,
    std::span<const TranscriptEntry> entries);

} // namespace cha::web
