#pragma once

#include "transcript/transcript.h"
#include "ui/render/transcript_writer.h"

#include <cstddef>
#include <optional>

namespace cha {

// Projects TranscriptSnapshot changes onto an append-only TranscriptSurface.
// Staged and committed positions let ConsoleSession flush output before
// acknowledging bytes as delivered.
class TranscriptEmitter {
public:
    TranscriptEmitter(TranscriptSurface& surface, bool show_addressing);

    void write(const TranscriptSnapshot& snapshot);
    void commit();

private:
    // The last successfully delivered point in one transcript history epoch,
    // including the suffix position of an entry that is still streaming.
    struct Position {
        bool initialized{};
        std::size_t history_epoch{};
        EntryId last_emitted_id{};
        std::optional<EntryId> open_id;
        std::size_t open_text_size{};
    };

    TranscriptSurface& surface_;
    bool show_addressing_{};
    Position committed_;
    Position staged_;
};

} // namespace cha
