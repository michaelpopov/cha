#pragma once

#include "characters/character_config.h"
#include "chat/character.h"
#include "session/controller_update.h"
#include "session/generation_status.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace cha::web {

enum class SessionLifecycle { starting, running, stopping };
enum class ShutdownReason {
    session_closed,
    reloading,
    session_failed,
    session_deleted,
    server_stopping,
    retired,
};
enum class ErrorCode {
    not_found, body_too_large, prompt_too_large,
    internal_error, speech_busy, vault_changed, session_stopping,
    session_limit_reached, session_open_timeout, server_stopping,
    session_not_live, command_timeout,
    command_queue_full, vault_password_required,
    source_vault_password_required,
    invalid_argument, operation_cancelled, application_unavailable,
};

struct SessionListing {
    SessionId id;
    std::string label;
    bool live{};
    std::int64_t updated_at{};
};

struct SpeechVoiceSettings {
    std::optional<double> speed;

    bool operator==(const SpeechVoiceSettings&) const = default;
};

struct SpeechVoice {
    std::string id;
    std::string display_name;
    std::string elevenlabs_voice_id;
    SpeechVoiceSettings settings;

    bool operator==(const SpeechVoice&) const = default;
};

// The lobby publishes the workspace-wide persona roster for discovery: the
// browser lists these and reads one persona's Markdown from
// /api/v1/personas/{id}. Transcript entries carry persona ids so the browser
// can preserve each stored prompt's attribution.
struct PersonaSummary {
    std::string id;
    std::string display_name;
    std::optional<std::string> description;
    CharacterAppearance appearance;
    std::optional<SpeechVoice> voice;
    bool operator==(const PersonaSummary&) const = default;
};

struct CharacterSummary {
    CharacterId id;
    std::string display_name;
    std::optional<std::string> description;
    // Always sent, defaults included, so the browser never has to decide what a
    // missing appearance means.
    CharacterAppearance appearance;
    std::optional<SpeechVoice> voice;
    bool operator==(const CharacterSummary&) const = default;
};

struct ForumSummary {
    ForumId id;
    std::string display_name;
    // The short line a roster row shows. The long FORUM.md description is
    // carried by ForumDetail, which the browser fetches only when it is read.
    // Discovery populates this; a session snapshot's forum leaves it unset,
    // because a session descriptor carries no description and chat shows none.
    std::optional<std::string> description;
    CharacterId default_character_id;
    std::string default_persona_id;
    std::string default_persona_display_name;
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
    std::optional<EntryId> covered_until;
    GenerationStatus generation;
    std::optional<std::string> notice;
    SessionLifecycle lifecycle{SessionLifecycle::starting};
    std::optional<ShutdownReason> shutdown_reason;
    std::set<EntryId> cached_audio_entries;
    bool operator==(const SessionSnapshot&) const = default;
};

// Author attribution is not carried here: LiveSession applies its current
// session persona, so a submitter cannot choose one.
struct RawCommand {
    std::string text;
};

struct StopCommand {};

struct CoverCommand {
    EntryId through_entry_id{};
};

struct UncoverCommand {};

struct DeleteTurnCommand {
    EntryId response_entry_id{};
};

struct SetDefaultCharacterCommand {
    CharacterId character_id;
};

struct RenameSessionCommand {
    std::string label;
};

// A snapshot request shares the owner queue with mutations so callers
// never read controller-owned state directly.
struct SnapshotCommand {};
struct SubscribeCommand {
    std::string connection_id;
    std::uint64_t context_epoch{};
    std::string subscription_id;
};
struct UnsubscribeCommand {
    std::string connection_id;
    std::uint64_t context_epoch{};
    std::string subscription_id;
};
struct SubscribeResult {
    std::string connection_id;
    std::uint64_t context_epoch{};
    std::string subscription_id;
};

using WebCommand = std::variant<
    RawCommand,
    StopCommand,
    CoverCommand,
    UncoverCommand,
    DeleteTurnCommand,
    SetDefaultCharacterCommand,
    RenameSessionCommand,
    SnapshotCommand,
    SubscribeCommand,
    UnsubscribeCommand>;

struct CommandResult {
    // Owner-thread effects stay in this in-process result. The JSON serializer
    // exposes only clear_input and session.notice to the browser.
    ControllerUpdate session;
    bool clear_input{};
    // A successful default-character command also carries the canonical ID so
    // the session owner can update the forum configuration before publishing.
    std::optional<CharacterId> persist_default_character_id;
};

struct CreateSessionSuccess {
    SessionId id;
    std::string label;
};

struct SessionLabelResult {
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
    ForumId initial_forum_id;
    SessionId initial_session_id;
    std::vector<PersonaSummary> personas;
    std::vector<CharacterSummary> characters;
    std::vector<ForumSummary> forums;
    std::vector<RecentSession> recent_sessions;
    std::string vault_name;
    std::vector<std::string> vaults;
};

struct ProviderOption {
    std::string id;
    std::string label;
    bool operator==(const ProviderOption&) const = default;
};

struct StyleOption {
    std::string id;
    std::string label;
    CharacterAppearance appearance;
    bool operator==(const StyleOption&) const = default;
};

struct VoiceOption {
    std::string id;
    std::string label;
    bool operator==(const VoiceOption&) const = default;
};

struct CreateCharacterRequest {
    std::string display_name;
    std::string description;
};

struct CreateForumRequest {
    std::string display_name;
    std::string persona_id;
};

struct CharacterSettingsUpdate {
    std::string provider;
    std::optional<std::string> style;
    std::optional<std::string> voice;
    std::optional<std::string> reasoning_effort;
    std::optional<WebSearchMode> web_search;
};

struct CharacterDefinitionUpdate {
    std::optional<std::string> display_name;
    std::optional<std::string> character_markdown;
};

struct CharacterDetail {
    CharacterSummary summary;
    std::string character_markdown;
    std::string editable_markdown;
    std::vector<std::string> markdown_files;
    std::optional<std::string> provider;
    std::optional<std::string> style;
    std::optional<std::string> voice;
    std::optional<std::string> reasoning_effort;
    std::optional<WebSearchMode> web_search;
    std::vector<ProviderOption> available_providers;
    std::vector<StyleOption> available_styles;
    std::vector<VoiceOption> available_voices;
    bool settings_writable{};
    bool writable{};
};

struct PersonaDetail {
    PersonaSummary summary;
    // PERSONA.md verbatim, and empty for a persona that configures none.
    std::string persona_markdown;
    std::optional<std::string> style;
    std::optional<std::string> voice;
    std::vector<StyleOption> available_styles;
    std::vector<VoiceOption> available_voices;
    bool writable{};
};

struct PersonaUpdate {
    std::optional<std::string> display_name;
    std::optional<std::string> persona_markdown;
    std::optional<std::optional<std::string>> style;
    std::optional<std::optional<std::string>> voice;
};

struct ForumUpdate {
    std::optional<std::string> display_name;
    std::optional<std::string> forum_markdown;
};

struct ForumMembersUpdate {
    std::vector<CharacterId> character_ids;
    std::string persona_id;
};

struct ForumDetail {
    ForumSummary summary;
    // FORUM.md verbatim, and empty for a forum that has none. The same file is
    // the forum's system prompt; publishing it whole is deliberate.
    std::string forum_markdown;
    std::vector<std::string> markdown_files;
    bool writable{};
};

struct MarkdownFile {
    std::string filename;
    std::string content;
    bool writable{};
};

struct SessionExport {
    std::string markdown;
};

struct ProviderSummary {
    std::string id;
    std::string display_name;
    std::string model;
    std::string host;
};

struct ProviderDetail {
    std::string id;
    std::string display_name;
    std::string host;
    int port{};
    std::string base_path;
    std::string mode;
    std::string model;
    bool stream{};
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    int timeout_s{};
    int idle_timeout_s{};
    std::optional<std::string> api_key;
    std::string reasoning_effort;
    std::string reasoning_format;
    bool https{};
    std::string api;
    std::string auth;
    std::string web_search;
    std::string cache_retention;
    std::vector<std::string> openrouter_targets;
    bool writable{};
    std::vector<std::string> used_by;
};

struct ProviderUpdate {
    std::string display_name;
    ModelBackendConfig config;
};

struct CreateProviderRequest {
    std::string display_name;
    std::string copy_from;
};

struct StyleDetail {
    std::string id;
    std::string display_name;
    CharacterAppearance appearance;
    bool writable{};
    std::vector<std::string> used_by;
};

struct StyleUpdate {
    std::string display_name;
    CharacterAppearance appearance;
};

struct VoiceDetail {
    std::string id;
    std::string display_name;
    std::string description;
    std::string elevenlabs_voice_id;
    std::optional<double> speed;
    bool writable{};
    std::vector<std::string> used_by;
};

struct VoiceUpdate {
    std::string display_name;
    std::string description;
    std::string elevenlabs_voice_id;
    SpeechVoiceSettings settings;
};

struct CreateVoiceRequest {
    std::string display_name;
    std::string description;
    std::string elevenlabs_voice_id;
};

struct VoiceInputSettings {
    std::string url;
    std::string model;
    std::string api_key;
    std::string delay;
    std::string prompt;
};

// Native runtime omits stored secrets. HTTP adapters may add a credential
// for the temporary JavaScript voice-input path.
struct VoiceInputRuntime {
    std::string url;
    std::string model;
    std::string delay;
    std::string prompt;
};

struct VoiceOutputSettings {
    std::string url;
    std::string model;
    std::string api_key;
    std::string output_format;
    std::string default_voice;
};

struct VoiceOutputRuntime {
    std::string url;
    std::string model;
    std::string output_format;
    std::string default_voice_id;
};

struct ApiKeyDetail {
    std::string id;
    std::string display_name;
    bool has_value{};
    std::vector<std::string> used_by;
};

struct CreateApiKeyRequest {
    std::string display_name;
    std::string value;
};

struct R2StorageDetail {
    std::string id;
    std::string display_name;
    std::string url;
    std::string access_key_id;
    bool has_secret_key{};
};

struct SaveR2StorageRequest {
    std::string display_name;
    std::string url;
    std::string access_key_id;
    std::optional<std::string> secret_key;
};

struct OpenAiAuth {
    std::string status;
    std::optional<std::string> user_code;
    std::optional<std::string> verification_url;
    std::optional<std::int64_t> attempt_expires_at;
    std::optional<std::int64_t> next_poll_delay_ms;
    std::optional<std::string> error;
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

// Whether coalesced output represented one controller-proven append exactly,
// or needs the owner to publish a current full snapshot instead.
enum class AppendPublishResult {
    Accepted,
    SnapshotRequired,
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
void to_json(nlohmann::json& json, const SessionLabelResult& value);
void to_json(nlohmann::json& json, const OpenSessionSuccess& value);
void to_json(nlohmann::json& json, const RecentSession& value);
void to_json(nlohmann::json& json, const Bootstrap& value);
void to_json(nlohmann::json& json, const ProviderOption& value);
void to_json(nlohmann::json& json, const StyleOption& value);
void to_json(nlohmann::json& json, const VoiceOption& value);
void to_json(nlohmann::json& json, const CharacterDetail& value);
void to_json(nlohmann::json& json, const PersonaDetail& value);
void to_json(nlohmann::json& json, const ForumDetail& value);
void to_json(nlohmann::json& json, const MarkdownFile& value);
void to_json(nlohmann::json& json, const SessionExport& value);
void to_json(nlohmann::json& json, const ProviderSummary& value);
void to_json(nlohmann::json& json, const ProviderDetail& value);
void to_json(nlohmann::json& json, const StyleDetail& value);
void to_json(nlohmann::json& json, const VoiceDetail& value);
void to_json(nlohmann::json& json, const VoiceInputSettings& value);
void to_json(nlohmann::json& json, const VoiceInputRuntime& value);
void to_json(nlohmann::json& json, const VoiceOutputSettings& value);
void to_json(nlohmann::json& json, const VoiceOutputRuntime& value);
void to_json(nlohmann::json& json, const ApiKeyDetail& value);
void to_json(nlohmann::json& json, const R2StorageDetail& value);
void to_json(nlohmann::json& json, const OpenAiAuth& value);
void to_json(nlohmann::json& json, const Error& value);
void to_json(nlohmann::json& json, const SnapshotEvent& value);
void to_json(nlohmann::json& json, const AppendEvent& value);

} // namespace cha::web
