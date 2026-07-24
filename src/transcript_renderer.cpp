#include "transcript_renderer.h"

#include <algorithm>
#include <cwchar>

namespace cha {
namespace {

void add_cells(int cells, int columns, int& rows, int& column) {
    for (int cell = 0; cell < cells; ++cell) {
        ++column;
        if (column >= columns) {
            ++rows;
            column = 0;
        }
    }
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

} // namespace

std::string transcript_entry_label(const ConversationEntry& entry, bool show_addressing) {
    switch (entry.kind) {
    case EntryKind::human:
        return show_addressing ? "[You → " + entry.addressed_to_name + "] " : "[You] ";
    case EntryKind::agent:
        return "[Agent: " + entry.display_name + "] ";
    case EntryKind::notice:
        return "[System] ";
    case EntryKind::error:
        return "[Error] ";
    }
    return "[Unknown] ";
}

void write_transcript_entry(
    TranscriptSurface& surface,
    const ConversationEntry& entry,
    bool show_addressing) {
    std::string label = transcript_entry_label(entry, show_addressing);
    if (entry.kind == EntryKind::agent && !entry.reasoning_text.empty()) {
        if (label.ends_with(' ')) {
            label.pop_back();
        }
        surface.attributes(TranscriptAttributes::bold);
        surface.write(label);
        surface.attributes(TranscriptAttributes::normal);
        surface.write("\n");
        surface.attributes(TranscriptAttributes::bold_dim);
        surface.write("[Reasoning]");
        surface.attributes(TranscriptAttributes::normal);
        surface.write("\n");
        surface.attributes(TranscriptAttributes::dim);
        surface.write(entry.reasoning_text);
        surface.attributes(TranscriptAttributes::normal);
        if (!entry.text.empty()) {
            surface.write("\n\n");
            surface.attributes(TranscriptAttributes::normal);
            surface.write(entry.text);
        }
    } else {
        surface.attributes(TranscriptAttributes::bold);
        surface.write(label);
        surface.attributes(TranscriptAttributes::normal);
        surface.write(entry.text);
    }
    surface.attributes(TranscriptAttributes::normal);
}

void write_transcript_suffix(
    TranscriptSurface& surface,
    CompletionDeltaKind kind,
    std::string_view text) {
    surface.attributes(
        kind == CompletionDeltaKind::reasoning
            ? TranscriptAttributes::dim
            : TranscriptAttributes::normal);
    surface.write(text);
    surface.attributes(TranscriptAttributes::normal);
}

void initialize_transcript_surface(TranscriptSurface& surface) {
    surface.attributes(TranscriptAttributes::normal);
}

TranscriptRenderPlan TranscriptRenderPlanner::plan(
    const ConversationSnapshot& snapshot,
    int columns
) const {
    if (!initialized_ || columns != columns_ || snapshot.history_epoch != history_epoch_
        || snapshot.entries.size() < entry_count_) {
        return {.action = TranscriptRenderAction::rebuild};
    }

    if (snapshot.revision == revision_) {
        return {};
    }

    if (last_entry_) {
        const ConversationEntry& old_last = *last_entry_;
        const ConversationEntry& new_last = snapshot.entries[entry_count_ - 1];
        if (snapshot.entries.size() == entry_count_ && new_last == old_last) {
            return {};
        }
        if (new_last.id != old_last.id
            || new_last.kind != old_last.kind
            || new_last.participant_id != old_last.participant_id
            || new_last.display_name != old_last.display_name
            || new_last.addressed_to != old_last.addressed_to
            || new_last.addressed_to_name != old_last.addressed_to_name
            || new_last.request_id != old_last.request_id
            || !starts_with(new_last.reasoning_text, old_last.reasoning_text)
            || !starts_with(new_last.text, old_last.text)) {
            return {.action = TranscriptRenderAction::rebuild};
        }

        const bool reasoning_grew =
            new_last.reasoning_text.size() != old_last.reasoning_text.size();
        const bool answer_grew =
            new_last.text.size() != old_last.text.size();
        if (reasoning_grew && answer_grew) {
            return {.action = TranscriptRenderAction::rebuild};
        }
        if (reasoning_grew && !new_last.text.empty()) {
            return {.action = TranscriptRenderAction::rebuild};
        }
        if (answer_grew
            && !old_last.reasoning_text.empty()
            && old_last.text.empty()) {
            return {.action = TranscriptRenderAction::rebuild};
        }

        const CompletionDeltaKind suffix_kind = reasoning_grew
            ? CompletionDeltaKind::reasoning
            : CompletionDeltaKind::answer;
        const std::string suffix = reasoning_grew
            ? new_last.reasoning_text.substr(old_last.reasoning_text.size())
            : new_last.text.substr(old_last.text.size());
        return {
            .action = TranscriptRenderAction::append,
            .resumes_last_message = true,
            .suffix_kind = suffix_kind,
            .last_message_suffix = suffix,
            .first_new_message = entry_count_,
        };
    }

    return {
        .action = TranscriptRenderAction::append,
        .first_new_message = 0,
    };
}

void TranscriptRenderPlanner::commit(const ConversationSnapshot& snapshot, int columns) {
    initialized_ = true;
    columns_ = columns;
    revision_ = snapshot.revision;
    history_epoch_ = snapshot.history_epoch;
    entry_count_ = snapshot.entries.size();
    if (snapshot.entries.empty()) {
        last_entry_.reset();
    } else {
        last_entry_ = snapshot.entries.back();
    }
}

void TranscriptViewport::update(int transcript_lines, int output_height) {
    transcript_lines_ = std::max(0, transcript_lines);
    output_height_ = std::max(1, output_height);
    if (follows_output_) {
        top_ = bottom();
    } else {
        top_ = std::min(top_, bottom());
    }
}

void TranscriptViewport::scroll_up() {
    follows_output_ = false;
    top_ = std::max(0, top_ - std::max(1, output_height_ / 2));
}

void TranscriptViewport::scroll_down() {
    top_ = std::min(bottom(), top_ + std::max(1, output_height_ / 2));
    follows_output_ = top_ == bottom();
}

int TranscriptViewport::top() const {
    return top_;
}

bool TranscriptViewport::follows_output() const {
    return follows_output_;
}

int TranscriptViewport::bottom() const {
    return std::max(0, transcript_lines_ - output_height_);
}

int layout_rows(std::string_view text, int columns, int initial_cells) {
    columns = std::max(1, columns);
    int result = 1;
    int column = 0;
    add_cells(initial_cells, columns, result, column);

    for (const unsigned char character : text) {
        if (character == '\n') {
            ++result;
            column = 0;
        } else if (character == '\r') {
            column = 0;
        } else if (character == '\t') {
            add_cells(8 - (column % 8), columns, result, column);
        } else {
            // Counting UTF-8 bytes is conservative, ensuring the pad is never too short.
            add_cells(1, columns, result, column);
        }
    }

    return result;
}

int layout_rows(std::wstring_view text, int columns, int initial_cells) {
    columns = std::max(1, columns);
    int result = 1;
    int column = 0;
    add_cells(initial_cells, columns, result, column);

    for (const wchar_t character : text) {
        if (character == L'\n') {
            ++result;
            column = 0;
        } else if (character == L'\r') {
            column = 0;
        } else if (character == L'\t') {
            add_cells(8 - (column % 8), columns, result, column);
        } else {
            const int width = ::wcwidth(character);
            add_cells(std::max(0, width), columns, result, column);
        }
    }

    return result;
}

} // namespace cha
