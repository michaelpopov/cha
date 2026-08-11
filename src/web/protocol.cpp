#include "web/protocol.h"

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace cha::web {
namespace {

template<typename Enum>
std::string_view enum_name(
    Enum value,
    std::initializer_list<std::pair<Enum, std::string_view>> names) {
    for (const auto& [candidate, name] : names) {
        if (candidate == value) {
            return name;
        }
    }
    throw std::invalid_argument("Invalid web protocol enum value");
}

template<typename T>
void put_optional(
    nlohmann::json& json,
    std::string_view name,
    const std::optional<T>& value) {
    if (value) {
        json[std::string(name)] = *value;
    }
}

nlohmann::json transcript_entry_json(const cha::TranscriptEntry& value) {
    nlohmann::json json = {
        {"id", value.id},
        {"kind", to_string(value.kind)},
        {"participant_id", value.participant_id},
        {"display_name", value.display_name},
        {"addressed_to", value.addressed_to},
        {"addressed_to_name", value.addressed_to_name},
        {"text", value.text},
        {"status", to_string(value.status)},
    };
    put_optional(json, "request_id", value.request_id);
    return json;
}

nlohmann::json generation_json(const GenerationStatus& value) {
    nlohmann::json json = {
        {"active", value.active},
        {"character_id", value.character_id},
        {"character_display_name", value.character_display_name},
        {"phase", to_string(value.phase)},
        {"reasoning_text", value.reasoning_text},
    };
    put_optional(json, "request_id", value.request_id);
    return json;
}

nlohmann::json append_target_json(const TextTarget& value) {
    return std::visit([](const auto& target) {
        using Target = std::decay_t<decltype(target)>;
        if constexpr (std::is_same_v<Target, EntryTextTarget>) {
            return nlohmann::json{
                {"kind", "entry"},
                {"entry_id", target.entry_id},
            };
        } else {
            return nlohmann::json{
                {"kind", "reasoning"},
                {"request_id", target.request_id},
            };
        }
    }, value);
}

} // namespace

std::string_view to_string(EntryKind value) {
    return enum_name(
        value,
        {
            {EntryKind::human, "human"},
            {EntryKind::character, "character"},
            {EntryKind::notice, "notice"},
            {EntryKind::error, "error"},
        });
}

std::string_view to_string(EntryStatus value) {
    return enum_name(
        value,
        {
            {EntryStatus::complete, "complete"},
            {EntryStatus::streaming, "streaming"},
            {EntryStatus::cancelled, "cancelled"},
            {EntryStatus::failed, "failed"},
        });
}

std::string_view to_string(ResponsePhase value) {
    return enum_name(
        value,
        {
            {ResponsePhase::waiting, "waiting"},
            {ResponsePhase::reasoning, "reasoning"},
            {ResponsePhase::answering, "answering"},
            {ResponsePhase::stopping, "stopping"},
        });
}

std::string_view to_string(SessionLifecycle value) {
    return enum_name(
        value,
        {
            {SessionLifecycle::starting, "starting"},
            {SessionLifecycle::running, "running"},
            {SessionLifecycle::stopping, "stopping"},
        });
}

std::string_view to_string(ShutdownReason value) {
    return enum_name(
        value,
        {
            {ShutdownReason::browser_disconnected, "browser_disconnected"},
            {ShutdownReason::session_failed, "session_failed"},
            {ShutdownReason::server_stopping, "server_stopping"},
        });
}

std::string_view to_string(ErrorCode value) {
    return enum_name(
        value,
        {
            {ErrorCode::not_found, "not_found"},
            {ErrorCode::bad_request, "bad_request"},
            {ErrorCode::body_too_large, "body_too_large"},
            {ErrorCode::prompt_too_large, "prompt_too_large"},
            {ErrorCode::forbidden_origin, "forbidden_origin"},
            {ErrorCode::internal_error, "internal_error"},
            {ErrorCode::session_busy, "session_busy"},
            {ErrorCode::session_stopping, "session_stopping"},
            {ErrorCode::session_limit_reached, "session_limit_reached"},
            {ErrorCode::session_open_timeout, "session_open_timeout"},
            {ErrorCode::server_stopping, "server_stopping"},
            {ErrorCode::session_not_live, "session_not_live"},
            {ErrorCode::browser_stream_in_use, "browser_stream_in_use"},
            {ErrorCode::command_timeout, "command_timeout"},
            {ErrorCode::command_queue_full, "command_queue_full"},
        });
}

std::optional<SnapshotAppendSelection> snapshot_append_selection(
    const SessionSnapshot& snapshot) {
    for (std::size_t index = 0; index != snapshot.transcript.size(); ++index) {
        const cha::TranscriptEntry& entry = snapshot.transcript[index];
        if (entry.status == EntryStatus::streaming) {
            return SnapshotAppendSelection{
                EntryTextTarget{entry.id}, index};
        }
    }
    if (snapshot.generation.active && snapshot.generation.request_id
        && snapshot.generation.phase == ResponsePhase::reasoning) {
        return SnapshotAppendSelection{
            ReasoningTextTarget{*snapshot.generation.request_id},
            std::nullopt};
    }
    return std::nullopt;
}

void to_json(nlohmann::json& json, const ForumSummary& value) {
    json = {
        {"id", value.id},
        {"display_name", value.display_name},
        {"default_character_id", value.default_character_id},
        {"default_persona_id", value.default_persona_id},
        {"default_persona_display_name", value.default_persona_display_name},
        {"members", value.members},
    };
}

void to_json(nlohmann::json& json, const PersonaSummary& value) {
    json = {
        {"id", value.id},
        {"display_name", value.display_name},
    };
    put_optional(json, "description", value.description);
}

void to_json(nlohmann::json& json, const SessionListing& value) {
    json = {
        {"id", value.id},
        {"label", value.label},
        {"live", value.live},
        {"updated_at", value.updated_at},
    };
}

void to_json(nlohmann::json& json, const CharacterSummary& value) {
    json = {
        {"id", value.id},
        {"display_name", value.display_name},
        {"appearance", {
            {"font", to_string(value.appearance.font)},
            {"style", to_string(value.appearance.style)},
            {"weight", to_string(value.appearance.weight)},
            {"size", to_string(value.appearance.size)},
        }},
    };
    put_optional(json, "description", value.description);
}

void to_json(nlohmann::json& json, const SessionSnapshot& value) {
    nlohmann::json transcript = nlohmann::json::array();
    for (const cha::TranscriptEntry& entry : value.transcript) {
        transcript.push_back(transcript_entry_json(entry));
    }
    json = {
        {"forum", value.forum},
        {"session_id", value.session_id},
        {"session_label", value.session_label},
        {"characters", value.characters},
        {"default_character_id", value.default_character_id},
        {"transcript", std::move(transcript)},
        {"generation", generation_json(value.generation)},
        {"lifecycle", to_string(value.lifecycle)},
    };
    put_optional(json, "notice", value.notice);
    if (value.shutdown_reason) {
        json["shutdown_reason"] = to_string(*value.shutdown_reason);
    }
}

void to_json(nlohmann::json& json, const CommandResult& value) {
    json = {{"clear_input", value.clear_input}};
    put_optional(json, "notice", value.session.notice);
}

void to_json(nlohmann::json& json, const CreateSessionSuccess& value) {
    json = {
        {"id", value.id},
        {"label", value.label},
    };
}

void to_json(nlohmann::json& json, const OpenSessionSuccess& value) {
    json = {{"forum_id", value.forum_id}, {"session_id", value.session_id}};
}

void to_json(nlohmann::json& json, const RecentSession& value) {
    json = {{"forum_id", value.forum_id}, {"session_id", value.session_id},
            {"session_label", value.session_label}, {"updated_at", value.updated_at}};
}

void to_json(nlohmann::json& json, const Bootstrap& value) {
    json = {{"initial_forum_id", value.initial_forum_id},
            {"initial_session_id", value.initial_session_id},
            {"characters", value.characters}, {"forums", value.forums},
            {"recent_sessions", value.recent_sessions}};
}

void to_json(nlohmann::json& json, const CharacterDetail& value) {
    json = nlohmann::json(value.summary);
    json["character_markdown"] = value.character_markdown;
}

void to_json(nlohmann::json& json, const Error& value) {
    json = {
        {"error",
         {
             {"code", to_string(value.code)},
             {"message", value.message},
         }},
    };
}

void to_json(nlohmann::json& json, const SnapshotEvent& value) {
    json = value.snapshot;
}

void to_json(nlohmann::json& json, const AppendEvent& value) {
    json = {
        {"target", append_target_json(value.target)},
        {"text", value.text},
        {"seq", value.seq},
    };
}

} // namespace cha::web
