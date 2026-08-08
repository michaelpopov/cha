#pragma once

#include "agents/config.h"
#include "session/controller_update.h"
#include "session/generation_status.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace cha::web {

class SseMailbox;

enum class SessionLifecycle { starting, running, stopping };
enum class ShutdownReason { browser_disconnected, session_failed, server_stopping };
enum class ErrorCode {
    not_found, bad_request, body_too_large, prompt_too_large, forbidden_host,
    forbidden_origin, internal_error, session_busy, session_stopping,
    session_limit_reached, session_open_timeout, server_stopping,
    session_not_live, browser_stream_in_use, command_timeout,
    command_queue_full,
};

// The lobby exposes identities only for browser-side attribution selection.
// A session snapshot deliberately does not carry this workspace-wide roster.
struct PersonaSummary {
    std::string id;
    std::string display_name;
    std::optional<std::string> description;
    bool operator==(const PersonaSummary&) const = default;
};

struct SessionListing {
    SessionId id;
    std::string label;
    bool live{};
    std::int64_t updated_at{};
};

struct CharacterSummary {
    CharacterId id;
    std::string display_name;
    std::optional<std::string> description;
    // Always sent, defaults included, so the browser never has to decide what a
    // missing appearance means.
    CharacterAppearance appearance;
    bool operator==(const CharacterSummary&) const = default;
};

struct ForumSummary {
    ForumId id;
    std::string display_name;
    CharacterId default_character_id;
    std::vector<CharacterSummary> members;
    bool operator==(const ForumSummary&) const = default;
};

struct SessionSnapshot {
    ForumSummary forum;
    SessionId session_id;
    std::string session_label;
    std::vector<CharacterSummary> characters;
    CharacterId default_character_id;
    std::vector<cha::TranscriptEntry> transcript;
    GenerationStatus generation;
    std::optional<std::string> notice;
    SessionLifecycle lifecycle{SessionLifecycle::starting};
    std::optional<ShutdownReason> shutdown_reason;
    bool operator==(const SessionSnapshot&) const = default;
};

struct RawCommand {
    // Stable author ID forwarded to the web text-input grammar.
    std::string persona;
    std::string text;
};

struct StopCommand {};

struct SetDefaultCharacterCommand {
    CharacterId character_id;
};

// A snapshot request shares the owner queue with mutations so HTTP threads
// never read controller-owned state directly.
struct SnapshotCommand {};
struct SseConnectCommand {};

struct SseStreamToken {
    std::uint64_t id{};
};

struct SseConnectResult {
    std::shared_ptr<SseMailbox> mailbox;
    SseStreamToken stream;
    std::uint64_t connection_id{};
};

using WebCommand = std::variant<
    RawCommand,
    StopCommand,
    SetDefaultCharacterCommand,
    SnapshotCommand,
    SseConnectCommand>;

struct CommandResult {
    // Owner-thread effects stay in this in-process result. The JSON serializer
    // exposes only clear_input and session.notice to the browser.
    ControllerUpdate session;
    bool clear_input{};
    bool close_session{};
};

struct CreateSessionSuccess {
    SessionId id;
    std::string label;
};

struct OpenSessionSuccess {
    ForumId forum_id;
    SessionId session_id;
};

struct RecentSession {
    ForumId forum_id;
    SessionId session_id;
    std::string session_label;
    std::int64_t updated_at{};
};

struct Bootstrap {
    std::string initial_persona_id;
    ForumId initial_forum_id;
    SessionId initial_session_id;
    std::vector<PersonaSummary> personas;
    std::vector<CharacterSummary> characters;
    std::vector<ForumSummary> forums;
    std::vector<RecentSession> recent_sessions;
};

struct CharacterDetail {
    CharacterSummary summary;
    std::string character_markdown;
};

struct Error {
    ErrorCode code{ErrorCode::internal_error};
    std::string message;
};

struct SnapshotAppendSelection {
    TextTarget target;
    std::optional<std::size_t> transcript_index;
};

// The snapshot is the authoritative base for later append events. Keep target
// selection shared by the session actor and its transport mailbox so they
// cannot establish different delta bases.
[[nodiscard]] std::optional<SnapshotAppendSelection> snapshot_append_selection(
    const SessionSnapshot& snapshot);

// Owning payload moved from a session owner thread into the SSE mailbox.
// Keeping the snapshot by value prevents the writer from borrowing live state.
struct SnapshotEvent {
    SessionSnapshot snapshot;
};

struct AppendEvent {
    TextTarget target;
    std::string text;
    std::uint64_t seq{};
};

std::string_view to_string(EntryKind value);
std::string_view to_string(EntryStatus value);
std::string_view to_string(ResponsePhase value);
std::string_view to_string(SessionLifecycle value);
std::string_view to_string(ShutdownReason value);
std::string_view to_string(ErrorCode value);

void to_json(nlohmann::json& json, const ForumSummary& value);
void to_json(nlohmann::json& json, const PersonaSummary& value);
void to_json(nlohmann::json& json, const SessionListing& value);
void to_json(nlohmann::json& json, const CharacterSummary& value);
void to_json(nlohmann::json& json, const SessionSnapshot& value);
void to_json(nlohmann::json& json, const CommandResult& value);
void to_json(nlohmann::json& json, const CreateSessionSuccess& value);
void to_json(nlohmann::json& json, const OpenSessionSuccess& value);
void to_json(nlohmann::json& json, const RecentSession& value);
void to_json(nlohmann::json& json, const Bootstrap& value);
void to_json(nlohmann::json& json, const CharacterDetail& value);
void to_json(nlohmann::json& json, const Error& value);
void to_json(nlohmann::json& json, const SnapshotEvent& value);
void to_json(nlohmann::json& json, const AppendEvent& value);

} // namespace cha::web
