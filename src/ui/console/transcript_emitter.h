#pragma once

#include "transcript/transcript.h"
#include "ui/render/transcript_writer.h"

#include <cstddef>
#include <optional>

namespace cha {

// Projects a call-scoped TranscriptView onto an append-only TranscriptSurface.
// Staged and committed indices let ConsoleSession flush output before
// acknowledging bytes as delivered; no transcript storage is retained.
class TranscriptEmitter {
public:
    // When echo_human_entries is false, only restored history still prints human
    // prompts; later interactive submissions are already visible via TTY echo.
    TranscriptEmitter(
        TranscriptSurface& surface,
        bool show_addressing,
        bool echo_human_entries = true);

    void write(TranscriptView transcript);
    void commit();

private:
    // The last successfully delivered point in one transcript history epoch,
    // including the suffix position of an entry that is still streaming.
    struct Position {
        bool initialized{};
        std::size_t history_epoch{};
        std::optional<EntryId> open_id;
        std::size_t open_text_size{};
        std::size_t next_entry_index{};
    };

    TranscriptSurface& surface_;
    bool show_addressing_{};
    bool echo_human_entries_{true};
    Position committed_;
    Position staged_;
};

} // namespace cha
