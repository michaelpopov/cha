#include "agents/completion_context.h"

#include "util/json_serialization.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <unordered_set>

namespace cha {
namespace {

using Json = nlohmann::ordered_json;

std::string encode_shared_entry(const TranscriptEntry& entry) {
    Json encoded;
    if (entry.kind == EntryKind::human) {
        encoded["kind"] = "human";
        encoded["speaker"] = entry.display_name;
        encoded["addressed_to"] = entry.addressed_to_name;
        encoded["text"] = entry.text;
    } else {
        encoded["kind"] = "character";
        encoded["speaker"] = entry.display_name;
        encoded["text"] = entry.text;
    }
    return dump_json(encoded, "Completion request");
}

std::string prefixed_human_message(
    std::string_view display_name,
    std::string_view text) {
    return "from " + std::string(display_name) + ":\n" + std::string(text);
}

} // namespace

std::vector<CompletionMessage> project_completion_context(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    OffrecordSpan offrecord_span,
    std::string_view system_prompt,
    std::string_view character_id) {
    std::vector<CompletionMessage> messages;
    if (!system_prompt.empty()) {
        messages.push_back({CompletionRole::system, std::string(system_prompt)});
    }

    std::unordered_set<RequestId> failed_requests;
    for (const TranscriptEntry& entry : entries) {
        if (entry.kind == EntryKind::error && entry.request_id) {
            failed_requests.insert(*entry.request_id);
        }
    }

    const auto projectable = [&failed_requests, open_entry_id, offrecord_span](
                                 const TranscriptEntry& entry) {
        if (open_entry_id && *open_entry_id == entry.id) return false;
        if (offrecord_span.contains(entry.id)) return false;
        if (entry.kind == EntryKind::notice || entry.kind == EntryKind::error) return false;
        if (entry.kind == EntryKind::human) {
            return !entry.request_id || !failed_requests.contains(*entry.request_id);
        }
        return entry.status == EntryStatus::complete && !entry.text.empty();
    };

    bool shared_history_open = false;
    for (const TranscriptEntry& entry : entries) {
        if (!projectable(entry)) continue;
        const bool shared =
            (entry.kind == EntryKind::human && entry.addressed_to != character_id)
            || (entry.kind == EntryKind::character
                && entry.participant_id != character_id);
        if (shared) {
            if (!shared_history_open) {
                messages.push_back({
                    CompletionRole::persona,
                    std::string(shared_history_heading) + "\n",
                });
                shared_history_open = true;
            } else {
                messages.back().content.push_back('\n');
            }
            messages.back().content.append(encode_shared_entry(entry));
            continue;
        }

        shared_history_open = false;
        if (entry.kind == EntryKind::human) {
            messages.push_back({
                CompletionRole::persona,
                prefixed_human_message(entry.display_name, entry.text),
            });
        } else {
            messages.push_back({CompletionRole::assistant, entry.text});
        }
    }
    return messages;
}

std::vector<CompletionMessage> project_completion_context(
    const CompletionInput& input,
    std::string_view system_prompt) {
    if (!input.history) {
        throw std::invalid_argument("Completion input requires history");
    }
    std::vector<CompletionMessage> messages = project_completion_context(
        input.history->entries,
        input.history->open_entry_id,
        input.history->offrecord_span,
        system_prompt,
        input.run.target.id);
    messages.push_back({
        CompletionRole::persona,
        prefixed_human_message(input.run.author.display_name, input.run.prompt_text),
    });
    return messages;
}

} // namespace cha
