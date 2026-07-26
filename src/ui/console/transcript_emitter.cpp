#include "ui/console/transcript_emitter.h"

#include <algorithm>

namespace cha {
namespace {

const TranscriptEntry* find_entry(
    const TranscriptSnapshot& snapshot,
    EntryId id) {
    const auto found = std::find_if(
        snapshot.entries.begin(),
        snapshot.entries.end(),
        [id](const TranscriptEntry& entry) { return entry.id == id; });
    return found == snapshot.entries.end() ? nullptr : &*found;
}

} // namespace

TranscriptEmitter::TranscriptEmitter(
    TranscriptSurface& surface,
    bool show_addressing,
    bool echo_human_entries)
    : surface_(surface),
      show_addressing_(show_addressing),
      echo_human_entries_(echo_human_entries) {
}

void TranscriptEmitter::write(const TranscriptSnapshot& snapshot) {
    staged_ = committed_;
    if (!staged_.initialized) {
        staged_.initialized = true;
        staged_.history_epoch = snapshot.history_epoch;
    } else if (snapshot.history_epoch != staged_.history_epoch) {
        if (staged_.open_id) {
            surface_.write("\n\n");
        }
        surface_.write("--- cleared ---\n\n");
        staged_.history_epoch = snapshot.history_epoch;
        staged_.last_emitted_id = 0;
        staged_.open_id.reset();
        staged_.open_text_size = 0;
    }

    if (staged_.open_id) {
        const EntryId open_id = *staged_.open_id;
        const TranscriptEntry* entry = find_entry(snapshot, open_id);
        if (entry && entry->text.size() > staged_.open_text_size) {
            write_transcript_suffix(
                surface_,
                std::string_view(entry->text).substr(staged_.open_text_size));
            staged_.open_text_size = entry->text.size();
        }
        if (!entry || snapshot.open_entry_id != open_id) {
            surface_.write("\n\n");
            staged_.last_emitted_id =
                std::max(staged_.last_emitted_id, open_id);
            staged_.open_id.reset();
            staged_.open_text_size = 0;
        }
    }

    for (const TranscriptEntry& entry : snapshot.entries) {
        if (entry.id <= staged_.last_emitted_id
            || (staged_.open_id && entry.id == *staged_.open_id)) {
            continue;
        }
        // Interactive terminals already echo typed input. After the first
        // emission (restored history), suppress live human prompts so they do
        // not appear a second time beneath "> ".
        if (!echo_human_entries_
            && entry.kind == EntryKind::human
            && committed_.initialized) {
            // Keep the blank line that used to follow the echoed prompt so the
            // next agent/notice block is not jammed against the typed input.
            surface_.write("\n");
            staged_.last_emitted_id = entry.id;
            continue;
        }
        write_transcript_entry(surface_, entry, show_addressing_);
        if (snapshot.open_entry_id == entry.id) {
            staged_.open_id = entry.id;
            staged_.open_text_size = entry.text.size();
        } else {
            surface_.write("\n\n");
            staged_.last_emitted_id = entry.id;
        }
    }
}

void TranscriptEmitter::commit() {
    committed_ = staged_;
}

} // namespace cha
