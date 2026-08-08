#include "agents/model_context.h"

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
    return dump_json(encoded, "Model request");
}

std::string prefixed_human_message(
    std::string_view display_name,
    std::string_view text) {
    return "from " + std::string(display_name) + ":\n" + std::string(text);
}

} // namespace

std::vector<ModelMessage> project_model_context(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    OffrecordSpan offrecord_span,
    std::string_view system_prompt,
    std::string_view character_id) {
    std::vector<ModelMessage> messages;
    if (!system_prompt.empty()) {
        messages.push_back({ModelRole::system, std::string(system_prompt)});
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
                    ModelRole::persona,
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
                ModelRole::persona,
                prefixed_human_message(entry.display_name, entry.text),
            });
        } else {
            messages.push_back({ModelRole::assistant, entry.text});
        }
    }
    return messages;
}

std::vector<ModelMessage> project_model_context(
    const GenerationRequest& input,
    std::string_view system_prompt) {
    if (!input.history) {
        throw std::invalid_argument("Generation request requires history");
    }
    std::vector<ModelMessage> messages = project_model_context(
        input.history->entries,
        input.history->open_entry_id,
        input.history->offrecord_span,
        system_prompt,
        input.run.target.id);
    messages.push_back({
        ModelRole::persona,
        prefixed_human_message(input.run.author.display_name, input.run.prompt_text),
    });
    return messages;
}

} // namespace cha
