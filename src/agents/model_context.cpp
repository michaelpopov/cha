#include "agents/model_context.h"

#include "util/json_serialization.h"

#include <nlohmann/json.hpp>

#include <array>
#include <ctime>
#include <stdexcept>
#include <unordered_set>

namespace cha {
namespace {

using Json = nlohmann::ordered_json;

std::string format_utc_timestamp(std::int64_t unix_seconds) {
    if (unix_seconds == 0) return {};

    const std::time_t time = static_cast<std::time_t>(unix_seconds);
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &time) != 0) return {};
#else
    if (gmtime_r(&time, &utc) == nullptr) return {};
#endif
    std::array<char, 21> text{};
    if (std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return {};
    }
    return text.data();
}

std::string encode_shared_entry(
    const TranscriptEntry& entry,
    bool include_timestamps) {
    Json encoded;
    if (entry.kind == EntryKind::human) {
        encoded["kind"] = "human";
        encoded["speaker"] = entry.display_name;
        encoded["addressed_to"] = entry.addressed_to_name;
        if (include_timestamps) {
            if (const std::string timestamp = format_utc_timestamp(entry.created_at);
                !timestamp.empty()) {
                encoded["created_at"] = timestamp;
            }
        }
        encoded["text"] = entry.text;
    } else {
        encoded["kind"] = "character";
        encoded["speaker"] = entry.display_name;
        if (include_timestamps) {
            if (const std::string timestamp = format_utc_timestamp(entry.created_at);
                !timestamp.empty()) {
                encoded["created_at"] = timestamp;
            }
        }
        encoded["text"] = entry.text;
    }
    return dump_json(encoded, "Model request");
}

std::string prefixed_human_message(
    std::string_view display_name,
    std::int64_t created_at,
    std::string_view text) {
    std::string result = "from " + std::string(display_name);
    if (const std::string timestamp = format_utc_timestamp(created_at); !timestamp.empty()) {
        result += " at " + timestamp;
    }
    return result + ":\n" + std::string(text);
}

std::string timestamped_character_message(
    std::int64_t created_at,
    std::string_view text) {
    const std::string timestamp = format_utc_timestamp(created_at);
    if (timestamp.empty()) return std::string(text);
    return "[" + timestamp + "]\n" + std::string(text);
}

std::vector<ModelMessage> project_entries(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    OffrecordSpan offrecord_span,
    std::string_view system_prompt,
    std::string_view character_id,
    bool include_timestamps) {
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
            messages.back().content.append(encode_shared_entry(entry, include_timestamps));
            continue;
        }

        shared_history_open = false;
        if (entry.kind == EntryKind::human) {
            messages.push_back({
                ModelRole::persona,
                prefixed_human_message(
                    entry.display_name,
                    include_timestamps ? entry.created_at : 0,
                    entry.text),
            });
        } else {
            messages.push_back({
                ModelRole::assistant,
                timestamped_character_message(
                    include_timestamps ? entry.created_at : 0,
                    entry.text),
            });
        }
    }
    return messages;
}

} // namespace

std::vector<ModelMessage> project_model_context(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    OffrecordSpan offrecord_span,
    std::string_view system_prompt,
    std::string_view character_id) {
    return project_entries(
        entries,
        open_entry_id,
        offrecord_span,
        system_prompt,
        character_id,
        false);
}

std::vector<ModelMessage> project_model_context(
    const GenerationRequest& input,
    std::string_view system_prompt) {
    if (!input.history) {
        throw std::invalid_argument("Generation request requires history");
    }
    const bool include_timestamps = input.run.created_at != 0;
    std::vector<ModelMessage> messages = project_entries(
        input.history->entries,
        input.history->open_entry_id,
        input.history->offrecord_span,
        system_prompt,
        input.run.target.id,
        include_timestamps);
    if (const std::string timestamp = format_utc_timestamp(input.run.created_at);
        !timestamp.empty()) {
        const std::string time_context =
            "Conversation timestamps are in UTC. The current prompt was submitted at "
            + timestamp + ".";
        if (!messages.empty() && messages.front().role == ModelRole::system) {
            messages.front().content += "\n\n" + time_context;
        } else {
            messages.insert(messages.begin(), {ModelRole::system, time_context});
        }
    }
    messages.push_back({
        ModelRole::persona,
        prefixed_human_message(
            input.run.author.display_name,
            input.run.created_at,
            input.run.prompt_text),
    });
    return messages;
}

} // namespace cha
