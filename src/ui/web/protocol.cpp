#include "ui/web/protocol.h"

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <stdexcept>
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

} // namespace

std::string_view to_string(TranscriptKind value) {
    return enum_name(
        value,
        {
            {TranscriptKind::human, "human"},
            {TranscriptKind::agent, "agent"},
            {TranscriptKind::notice, "notice"},
            {TranscriptKind::error, "error"},
        });
}

std::string_view to_string(TranscriptStatus value) {
    return enum_name(
        value,
        {
            {TranscriptStatus::complete, "complete"},
            {TranscriptStatus::streaming, "streaming"},
            {TranscriptStatus::cancelled, "cancelled"},
            {TranscriptStatus::failed, "failed"},
        });
}

std::string_view to_string(GenerationPhase value) {
    return enum_name(
        value,
        {
            {GenerationPhase::waiting, "waiting"},
            {GenerationPhase::reasoning, "reasoning"},
            {GenerationPhase::answering, "answering"},
            {GenerationPhase::stopping, "stopping"},
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
            {ErrorCode::forbidden_host, "forbidden_host"},
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
        const TranscriptEntry& entry = snapshot.transcript[index];
        if (entry.status == TranscriptStatus::streaming) {
            return SnapshotAppendSelection{
                AppendTargetEntry{entry.id}, index};
        }
    }
    if (snapshot.generation.active && snapshot.generation.request_id
        && snapshot.generation.phase == GenerationPhase::reasoning) {
        return SnapshotAppendSelection{
            AppendTargetReasoning{*snapshot.generation.request_id},
            std::nullopt};
    }
    return std::nullopt;
}

void to_json(nlohmann::json& json, const ForumSummary& value) {
    json = {
        {"id", value.id},
        {"display_name", value.display_name},
    };
}

void to_json(nlohmann::json& json, const UserSummary& value) {
    json = {
        {"id", value.id},
        {"display_name", value.display_name},
    };
}

void to_json(nlohmann::json& json, const SessionListing& value) {
    json = {
        {"id", value.id},
        {"label", value.label},
        {"live", value.live},
    };
}

void to_json(nlohmann::json& json, const PersonaSummary& value) {
    json = {
        {"id", value.id},
        {"display_name", value.display_name},
    };
}

void to_json(nlohmann::json& json, const TranscriptEntry& value) {
    json = {
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
}

void to_json(nlohmann::json& json, const GenerationState& value) {
    json = {
        {"active", value.active},
        {"agent_id", value.agent_id},
        {"agent_name", value.agent_name},
        {"phase", to_string(value.phase)},
        {"reasoning_text", value.reasoning_text},
    };
    put_optional(json, "request_id", value.request_id);
}

void to_json(nlohmann::json& json, const SessionSnapshot& value) {
    json = {
        {"forum", value.forum},
        {"session_id", value.session_id},
        {"session_label", value.session_label},
        {"personas", value.personas},
        {"default_persona_id", value.default_persona_id},
        {"transcript", value.transcript},
        {"generation", value.generation},
        {"lifecycle", to_string(value.lifecycle)},
    };
    put_optional(json, "notice", value.notice);
    if (value.shutdown_reason) {
        json["shutdown_reason"] = to_string(*value.shutdown_reason);
    }
}

void to_json(nlohmann::json& json, const CommandResult& value) {
    json = {{"clear_input", value.clear_input}};
    put_optional(json, "notice", value.notice);
}

void to_json(nlohmann::json& json, const CreateSessionSuccess& value) {
    json = {
        {"id", value.id},
        {"label", value.label},
    };
}

void to_json(nlohmann::json& json, const OpenSessionSuccess& value) {
    json = {{"path", value.path}};
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

void to_json(nlohmann::json& json, const AppendTargetEntry& value) {
    json = {
        {"kind", "entry"},
        {"entry_id", value.entry_id},
    };
}

void to_json(nlohmann::json& json, const AppendTargetReasoning& value) {
    json = {
        {"kind", "reasoning"},
        {"request_id", value.request_id},
    };
}

void to_json(nlohmann::json& json, const SnapshotEvent& value) {
    json = value.snapshot;
}

void to_json(nlohmann::json& json, const AppendEvent& value) {
    json = {
        {"target",
         std::visit(
             [](const auto& target) {
                 return nlohmann::json(target);
             },
             value.target)},
        {"text", value.text},
        {"seq", value.seq},
    };
}

} // namespace cha::web
