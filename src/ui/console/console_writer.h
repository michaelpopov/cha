#pragma once

#include "ui/render/transcript_writer.h"

#include <iosfwd>
#include <string>
#include <string_view>

namespace cha {

// Implements the shared transcript-writing surface on an ostream, translating
// trusted attributes to optional ANSI SGR while neutralizing controls in
// untrusted transcript text. Successive write() calls are one continuous stream,
// so a C1 control split across two of them is still neutralized.
class ConsoleSurface final : public TranscriptSurface {
public:
    ConsoleSurface(std::ostream& output, bool color);
    ~ConsoleSurface() override = default;

    ConsoleSurface(const ConsoleSurface&) = delete;
    ConsoleSurface& operator=(const ConsoleSurface&) = delete;

    void attributes(TranscriptAttributes value) override;
    void write(std::string_view text) override;
    // Ends the untrusted byte stream, emitting an incomplete trailing lead byte.
    // The owner must perform a checked stream flush after this call.
    void finish();

private:
    std::ostream& output_;
    bool color_{};
    // A trailing byte withheld from the previous write because it may begin a
    // UTF-8 C1 control that the next write completes.
    bool held_lead_{};
};

// Writes the interactive target marker and restores normal attributes.
void write_console_prompt(
    TranscriptSurface& surface,
    std::string_view agent_name);

// Produces terminal-safe text for startup listings and any other whole string.
std::string sanitize_console_text(std::string_view text);

// The streaming form: neutralizes one chunk of a longer byte stream, withholding
// a trailing C1 lead byte and setting held_lead so the next call can decide it.
// C1 is the only rule spanning two bytes, and U+009B is an alternative CSI
// introducer, so deciding it per chunk would let a split sequence through.
std::string sanitize_console_chunk(std::string_view text, bool& held_lead);

} // namespace cha
