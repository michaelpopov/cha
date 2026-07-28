#include "ui/console/transcript_emitter.h"

namespace cha {

TranscriptEmitter::TranscriptEmitter(
    TranscriptSurface& surface,
    bool show_addressing,
    bool echo_human_entries)
    : surface_(surface),
      show_addressing_(show_addressing),
      echo_human_entries_(echo_human_entries) {
}

void TranscriptEmitter::write(TranscriptView transcript) {
    staged_ = committed_;
    if (!staged_.initialized) {
        staged_.initialized = true;
        staged_.history_epoch = transcript.history_epoch;
    } else if (transcript.history_epoch != staged_.history_epoch) {
        if (staged_.open_id) {
            surface_.write("\n\n");
        }
        surface_.write("--- cleared ---\n\n");
        staged_.history_epoch = transcript.history_epoch;
        staged_.open_id.reset();
        staged_.open_text_size = 0;
        staged_.next_entry_index = 0;
    }

    if (staged_.open_id) {
        const EntryId open_id = *staged_.open_id;
        const TranscriptEntry* entry =
            staged_.next_entry_index < transcript.size()
            && transcript.entries[staged_.next_entry_index].id == open_id
                ? &transcript.entries[staged_.next_entry_index]
                : nullptr;
        if (entry && entry->text.size() > staged_.open_text_size) {
            write_transcript_suffix(
                surface_,
                std::string_view(entry->text).substr(
                    staged_.open_text_size));
            staged_.open_text_size = entry->text.size();
        }
        if (!entry || transcript.open_entry_id != open_id) {
            surface_.write("\n\n");
            staged_.open_id.reset();
            staged_.open_text_size = 0;
            if (entry) {
                ++staged_.next_entry_index;
            }
        } else {
            return;
        }
    }

    while (staged_.next_entry_index < transcript.size()) {
        const TranscriptEntry& entry =
            transcript.entries[staged_.next_entry_index];
        // Interactive terminals already echo typed input. After the first
        // emission (restored history), suppress live human prompts so they do
        // not appear a second time beneath "> ".
        if (!echo_human_entries_
            && entry.kind == EntryKind::human
            && committed_.initialized) {
            // Keep the blank line that used to follow the echoed prompt so the
            // next agent/notice block is not jammed against the typed input.
            surface_.write("\n");
            ++staged_.next_entry_index;
            continue;
        }
        write_transcript_entry(surface_, entry, show_addressing_);
        if (transcript.open_entry_id == entry.id) {
            staged_.open_id = entry.id;
            staged_.open_text_size = entry.text.size();
            break;
        }
        surface_.write("\n\n");
        ++staged_.next_entry_index;
    }
}

void TranscriptEmitter::commit() {
    committed_ = staged_;
}

} // namespace cha
