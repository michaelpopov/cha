#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace cha::web {

enum class TranscriptKind { human, agent, notice, error };
enum class TranscriptStatus { complete, streaming, cancelled, failed };
enum class GenerationPhase { waiting, reasoning, answering, stopping };
enum class SessionLifecycle { starting, running, stopping };
enum class ShutdownReason { browser_disconnected, session_failed, server_stopping };
enum class ErrorCode {
    not_found, bad_request, body_too_large, forbidden_origin, internal_error,
    session_busy, session_stopping, session_limit_reached, session_open_timeout,
    server_stopping, session_not_live, browser_stream_in_use, command_timeout,
    command_queue_full,
};

struct ForumSummary {
    std::string id;
    std::string display_name;
    bool operator==(const ForumSummary&) const = default;
};

struct SessionListing {
    std::string id;
    std::string label;
    bool live{};
};

struct PersonaSummary {
    std::string id;
    std::string display_name;
    bool operator==(const PersonaSummary&) const = default;
};

struct TranscriptEntry {
    std::uint64_t id{};
    TranscriptKind kind{TranscriptKind::notice};
    std::string participant_id;
    std::string display_name;
    std::string addressed_to;
    std::string addressed_to_name;
    std::string text;
    TranscriptStatus status{TranscriptStatus::complete};
    std::optional<std::uint64_t> request_id;
    bool operator==(const TranscriptEntry&) const = default;
};

struct GenerationState {
    bool active{};
    std::optional<std::uint64_t> request_id;
    std::string agent_id;
    std::string agent_name;
    GenerationPhase phase{GenerationPhase::waiting};
    std::string reasoning_text;
    bool operator==(const GenerationState&) const = default;
};

struct SessionSnapshot {
    ForumSummary forum;
    std::string session_id;
    std::string session_label;
    std::vector<PersonaSummary> personas;
    std::string default_persona_id;
    std::vector<TranscriptEntry> transcript;
    GenerationState generation;
    std::optional<std::string> notice;
    SessionLifecycle lifecycle{SessionLifecycle::starting};
    std::optional<ShutdownReason> shutdown_reason;
    bool operator==(const SessionSnapshot&) const = default;
};

struct RawCommand {
    std::string text;
};

struct StopCommand {};

struct SetDefaultAgentCommand {
    std::string persona_id;
};

using WebCommand = std::variant<RawCommand, StopCommand, SetDefaultAgentCommand>;

struct CommandResult {
    bool clear_input{};
    std::optional<std::string> notice;
};

struct CreateSessionSuccess {
    std::string id;
    std::string label;
};

struct OpenSessionSuccess {
    std::string path;
};

struct Error {
    ErrorCode code{ErrorCode::internal_error};
    std::string message;
};

struct AppendTargetEntry {
    std::uint64_t entry_id{};
};

struct AppendTargetReasoning {
    std::uint64_t request_id{};
};

using AppendTarget = std::variant<AppendTargetEntry, AppendTargetReasoning>;

// Owning payload moved from a session owner thread into the SSE mailbox.
// Keeping the snapshot by value prevents the writer from borrowing live state.
struct SnapshotEvent {
    SessionSnapshot snapshot;
};

struct AppendEvent {
    AppendTarget target;
    std::string text;
    std::uint64_t seq{};
};

std::string_view to_string(TranscriptKind value);
std::string_view to_string(TranscriptStatus value);
std::string_view to_string(GenerationPhase value);
std::string_view to_string(SessionLifecycle value);
std::string_view to_string(ShutdownReason value);
std::string_view to_string(ErrorCode value);

void to_json(nlohmann::json& json, const ForumSummary& value);
void to_json(nlohmann::json& json, const SessionListing& value);
void to_json(nlohmann::json& json, const PersonaSummary& value);
void to_json(nlohmann::json& json, const TranscriptEntry& value);
void to_json(nlohmann::json& json, const GenerationState& value);
void to_json(nlohmann::json& json, const SessionSnapshot& value);
void to_json(nlohmann::json& json, const CommandResult& value);
void to_json(nlohmann::json& json, const CreateSessionSuccess& value);
void to_json(nlohmann::json& json, const OpenSessionSuccess& value);
void to_json(nlohmann::json& json, const Error& value);
void to_json(nlohmann::json& json, const AppendTargetEntry& value);
void to_json(nlohmann::json& json, const AppendTargetReasoning& value);
void to_json(nlohmann::json& json, const SnapshotEvent& value);
void to_json(nlohmann::json& json, const AppendEvent& value);

} // namespace cha::web
