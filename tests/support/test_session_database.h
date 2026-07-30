#pragma once

#include "session/session_database.h"

#include <filesystem>
#include <vector>

namespace cha {

// Test-only shorthand for assertions about the durable transcript payload.
inline std::vector<TranscriptEntry> load_transcript_entries(
    const std::filesystem::path& path) {
    return load_session_state(path).entries;
}

} // namespace cha
