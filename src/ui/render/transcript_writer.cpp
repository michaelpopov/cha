#include "ui/render/transcript_writer.h"

namespace cha {

bool show_addressing(
    const ForumPersonas& personas,
    const Transcript& transcript) {
    if (personas.all().size() > 1) {
        return true;
    }
    const std::string_view sole = personas.first().id;
    for (const TranscriptEntry& entry : transcript.entries()) {
        const bool foreign =
            (entry.kind == EntryKind::agent && entry.participant_id != sole)
            || (entry.kind == EntryKind::human && entry.addressed_to != sole);
        if (foreign) {
            return true;
        }
    }
    return false;
}

std::string transcript_entry_label(const TranscriptEntry& entry, bool show_addressing) {
    switch (entry.kind) {
    case EntryKind::human:
        return show_addressing ? "[You → " + entry.addressed_to_name + "] " : "[You] ";
    case EntryKind::agent:
        return "[" + entry.display_name + "] ";
    case EntryKind::notice:
        return "[System] ";
    case EntryKind::error:
        return "[Error] ";
    }
    return "[Unknown] ";
}

void write_transcript_entry(
    TranscriptSurface& surface,
    const TranscriptEntry& entry,
    bool show_addressing) {
    surface.attributes(TranscriptAttributes::bold);
    surface.write(transcript_entry_label(entry, show_addressing));
    surface.attributes(TranscriptAttributes::normal);
    surface.write(entry.text);
    surface.attributes(TranscriptAttributes::normal);
}

void write_active_response(
    TranscriptSurface& surface,
    std::string_view agent_name,
    std::string_view reasoning_text,
    std::string_view answer_text) {
    surface.attributes(TranscriptAttributes::bold);
    surface.write("[");
    surface.write(agent_name);
    surface.write("]\n");
    surface.attributes(TranscriptAttributes::bold_dim);
    surface.write("[Reasoning]");
    surface.attributes(TranscriptAttributes::normal);
    surface.write("\n");
    surface.attributes(TranscriptAttributes::dim);
    surface.write(reasoning_text);
    surface.attributes(TranscriptAttributes::normal);
    if (!answer_text.empty()) {
        surface.write("\n\n");
        surface.write(answer_text);
    }
    surface.attributes(TranscriptAttributes::normal);
}

void write_transcript_suffix(
    TranscriptSurface& surface,
    std::string_view text) {
    surface.attributes(TranscriptAttributes::normal);
    surface.write(text);
    surface.attributes(TranscriptAttributes::normal);
}

void initialize_transcript_surface(TranscriptSurface& surface) {
    surface.attributes(TranscriptAttributes::normal);
}

} // namespace cha
