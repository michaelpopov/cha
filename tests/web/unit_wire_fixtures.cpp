#include "web/protocol.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <stdexcept>
#include <string>

namespace cha::web {
namespace {

nlohmann::json load_fixture(std::string_view name) {
    const std::string path =
        std::string(CHA_WIRE_FIXTURE_DIRECTORY) + "/" + std::string(name);
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Missing wire fixture " + path);
    }
    nlohmann::json value;
    input >> value;
    return value;
}

TEST(WebWireFixtures, SerializesBootstrapSnapshotCommandsErrorsAndAppends) {
    const CharacterSummary assistant{
        "assistant", "Assistant", "CHA application guide"};
    const ForumSummary entrance{
        "entrance",
        "Entrance",
        std::nullopt,
        "assistant",
        "guest",
        "Guest",
        {assistant},
    };

    EXPECT_EQ(
        nlohmann::json(Bootstrap{
            .initial_forum_id = "entrance",
            .initial_session_id = "welcome",
            .personas = {{"guest", "Guest", "The built-in visitor persona"}},
            .characters = {assistant},
            .forums = {entrance},
            .recent_sessions = {{"entrance", "welcome", "Welcome", 2}},
            .vault_name = "Personal",
            .vaults = {"Personal"},
        }),
        load_fixture("bootstrap.json"));

    SessionSnapshot snapshot;
    snapshot.forum = entrance;
    snapshot.session_id = "welcome";
    snapshot.session_label = "Welcome";
    snapshot.characters = {assistant};
    snapshot.default_character_id = "assistant";
    snapshot.transcript = {{
        .id = 1,
        .kind = EntryKind::human,
        .participant_id = "guest",
        .display_name = "Guest",
        .addressed_to = "assistant",
        .addressed_to_name = "Assistant",
        .text = "Hello",
        .status = EntryStatus::complete,
        .created_at = 1700000000,
    }};
    snapshot.lifecycle = SessionLifecycle::running;
    snapshot.cached_audio_entries = {1};
    EXPECT_EQ(nlohmann::json(snapshot), load_fixture("snapshot.json"));

    EXPECT_EQ(
        nlohmann::json(AppendEvent{EntryTextTarget{1}, "Hello", 0}),
        load_fixture("append-entry.json"));
    EXPECT_EQ(
        nlohmann::json(AppendEvent{ReasoningTextTarget{7}, "Considering the request.", 1}),
        load_fixture("append-reasoning.json"));
    EXPECT_EQ(
        nlohmann::json(CommandResult{
            .session = {.notice = std::string("Saved")},
            .clear_input = true,
        }),
        load_fixture("command-result.json"));
    EXPECT_EQ(
        nlohmann::json(Error{
            ErrorCode::command_timeout, "<script>alert(1)</script>"}),
        load_fixture("error.json"));
    EXPECT_EQ(
        nlohmann::json(CharacterDetail{
            .summary = {"guide", "Guide"},
            .character_markdown = "Prompt",
            .editable_markdown = "Source",
            .provider = "terra",
            .style = std::nullopt,
            .voice = "brian",
            .reasoning_effort = "high",
            .web_search = WebSearchMode::automatic,
            .available_providers = {{"terra", "Terra"}},
            .available_styles = {{"serif-italic", "Serif italic",
                 {CharacterFont::serif, CharacterSlant::italic,
                  CharacterWeight::normal, CharacterScale::normal}}},
            .available_voices = {{"brian", "Brian"}},
            .settings_writable = true,
            .writable = true,
        }),
        load_fixture("character-detail.json"));

    EXPECT_EQ(
        nlohmann::json(ProviderDetail{
            .id = "test",
            .display_name = "Test",
            .host = "test",
            .port = 1,
            .base_path = "",
            .mode = "test",
            .model = "fake",
            .stream = true,
            .timeout_s = 600,
            .idle_timeout_s = 60,
            .reasoning_format = "auto",
            .api = "responses",
            .auth = "none",
            .web_search = "off",
            .cache_retention = "short",
            .writable = true,
            .used_by = {"Guide"},
        }),
        load_fixture("provider-detail.json"));
    EXPECT_EQ(
        nlohmann::json(ApiKeyDetail{
            .id = "api_key_1",
            .display_name = "Router",
            .has_value = true,
            .used_by = {"Test"},
        }),
        load_fixture("api-key-detail.json"));
    EXPECT_EQ(
        nlohmann::json(OpenAiAuth{
            .status = "waiting",
            .user_code = "ABCD-EFGH",
            .verification_url = "https://auth.openai.com/codex/device",
            .attempt_expires_at = 1700000900,
            .next_poll_delay_ms = 1000,
        }),
        load_fixture("openai-auth-status.json"));
    EXPECT_EQ(
        nlohmann::json(VoiceInputRuntime{
            .url = "https://api.openai.com/v1/realtime",
            .model = "gpt-4o-transcribe",
            .delay = "low",
            .prompt = "",
        }),
        load_fixture("voice-input-runtime.json"));
    EXPECT_EQ(
        nlohmann::json(R2StorageDetail{
            .id = "api_key_1",
            .display_name = "Backups",
            .url = "https://account.example/bucket",
            .access_key_id = "access-one",
            .has_secret_key = true,
        }),
        load_fixture("r2-storage-detail.json"));
}

} // namespace
} // namespace cha::web
