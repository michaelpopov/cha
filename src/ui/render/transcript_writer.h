#pragma once

#include "session/forum_characters.h"
#include "transcript/transcript.h"

#include <string>
#include <string_view>

namespace cha {

enum class TranscriptAttributes {
    normal,
    bold,
    dim,
    bold_dim,
};

// A sink for attributed transcript text. It keeps entry formatting independent
// of any front end, so the writers serve both applications and recording tests.
class TranscriptSurface {
public:
    virtual ~TranscriptSurface() = default;
    virtual void attributes(TranscriptAttributes value) = 0;
    virtual void write(std::string_view text) = 0;
};

// Writes one entry's labeled content and always restores normal attributes.
void write_transcript_entry(
    TranscriptSurface& surface,
    const TranscriptEntry& entry,
    bool show_addressing);
// Writes ephemeral reasoning followed by any answer text currently present. This is a
// presentation value only; reasoning never enters TranscriptEntry.
void write_active_response(
    TranscriptSurface& surface,
    std::string_view agent_name,
    std::string_view reasoning_text,
    std::string_view answer_text);
void write_transcript_suffix(
    TranscriptSurface& surface,
    std::string_view text);
void initialize_transcript_surface(TranscriptSurface& surface);

// Whether transcript labels should include routing (multi-agent forum or foreign history).
bool show_addressing(
    const ForumCharacters& characters,
    TranscriptView transcript);

// Produces an unambiguous display label from an entry's semantic kind.
std::string transcript_entry_label(const TranscriptEntry& entry, bool show_addressing);

} // namespace cha
