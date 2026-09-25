#include "workspace/workspace.h"
#include "workspace/workspace_config_editor.h"

#include "characters/model_context.h"
#include "support/test_workspace.h"
#include "util/logging.h"

#include <gtest/gtest.h>

#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cha {
namespace {

// These serialization tests reload directory fixtures. Runtime edits themselves
// only change the candidate map; apply that map to disk here for the next load.
template<typename Edit>
void edit_fixture(const Workspace& workspace, Edit&& edit) {
    TextFiles files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(workspace.root())) {
        if (!entry.is_regular_file() || entry.is_symlink()) continue;
        const auto content = TextSource{}.read(entry.path());
        files.emplace(entry.path().lexically_relative(workspace.root()).generic_string(), *content);
    }
    const auto before = files;
    WorkspaceConfigEditor editor(workspace, files);
    edit(editor);
    for (const auto& [name, content] : before) {
        if (files.contains(name)) continue;
        const auto path = workspace.root() / name;
        std::filesystem::remove(path);
        auto parent = path.parent_path();
        while (parent.parent_path() != workspace.root()
               && std::filesystem::is_empty(parent)) {
            std::filesystem::remove(parent);
            parent = parent.parent_path();
        }
    }
    for (const auto& [name, content] : files) {
        if (const auto old = before.find(name); old != before.end() && old->second == content) continue;
        const auto path = workspace.root() / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
    }
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

Workspace workspace_with_characters(
    std::initializer_list<std::pair<std::string_view, std::string_view>>
        characters) {
    test::TestWorkspace fixture;
    std::filesystem::remove_all(
        fixture.root() / "characters" / "guide");
    std::filesystem::remove_all(
        fixture.root() / "forums" / "lobby" / "members" / "guide");
    for (const auto& [id, display_name] : characters) {
        fixture.add_character(id, display_name);
        std::filesystem::create_directories(
            fixture.root() / "forums" / "lobby" / "members"
                / std::string(id));
    }
    return Workspace::load(fixture.root());
}

TEST(Workspace, EagerlyLoadsOwnedResolvedData) {
    test::TestWorkspace fixture;
    fixture.write_style("serif", "font = \"serif\"\nweight = \"bold\"\n");
    fixture.write_voice(
        "warm-narrator",
        "display_name = \"Warm Narrator\"\n"
        "description = \"Deep, resonant, comforting\"\n"
        "elevenlabs_voice_id = \"eleven-voice-123\"\n"
        "stability = 0.45\n"
        "similarity_boost = 0.8\n"
        "style = 0.2\n"
        "use_speaker_boost = true\n"
        "speed = 0.95\n");
    std::ofstream(
        fixture.root() / "system" / "assistant" / "character.toml")
        << "display_name = \"Assistant\"\n"
           "provider = \"test\"\n"
           "voice = \"warm-narrator\"\n";
    fixture.write_character_config(
        "display_name = \"Guide\"\n"
        "description = \"A helpful guide.\"\n"
        "provider = \"test\"\n"
        "style = \"serif\"\n"
        "voice = \"warm-narrator\"\n"
        "tags = [\"help\"]\n"
        "[prompt]\n"
        "greeting = \"Hello\"\n");
    std::ofstream(fixture.root() / "personas" / "reader" / "persona.toml")
        << "display_name = \"Reader\"\n"
           "style = \"serif\"\n"
           "voice = \"warm-narrator\"\n";
    std::ofstream(
        fixture.root() / "forums" / "lobby" / "members"
            / "character_defaults.toml")
        << "[prompt]\ngreeting = \"Welcome\"\n";
    std::ofstream(
        fixture.root() / "characters" / "guide" / "CHARACTER.md",
        std::ios::binary)
        << "$${greeting}, I am $${character.display_name} in "
           "$${forum.display_name}.\n"
           "<character_profile>\nIntrinsic Guide.\n</character_profile>\n";

    const Workspace workspace = Workspace::load(fixture.root());

    ASSERT_NE(workspace.find_provider("test"), nullptr);
    EXPECT_EQ(workspace.find_provider("test")->config.model, "fake");
    EXPECT_EQ(workspace.find_provider("test")->label, "Test");
    ASSERT_NE(workspace.find_style("serif"), nullptr);
    EXPECT_EQ(workspace.find_style("serif")->label, "Serif");
    EXPECT_EQ(
        workspace.find_style("serif")->appearance.font,
        CharacterFont::serif);
    const WorkspaceVoice* const voice = workspace.find_voice("warm-narrator");
    ASSERT_NE(voice, nullptr);
    EXPECT_EQ(voice->label, "Warm Narrator");
    EXPECT_EQ(voice->description, "Deep, resonant, comforting");
    EXPECT_EQ(voice->elevenlabs_voice_id, "eleven-voice-123");
    EXPECT_EQ(voice->settings.speed, 0.95);
    const WorkspacePersona* const persona = workspace.find_persona("reader");
    ASSERT_NE(persona, nullptr);
    EXPECT_EQ(persona->style_id, "serif");
    EXPECT_EQ(persona->voice_id, "warm-narrator");
    EXPECT_EQ(persona->appearance.font, CharacterFont::serif);
    EXPECT_EQ(persona->appearance.weight, CharacterWeight::bold);
    ASSERT_NE(workspace.find_character("guide"), nullptr);
    EXPECT_EQ(workspace.find_character("guide")->provider_id, "test");
    EXPECT_EQ(workspace.find_character("guide")->voice_id, "warm-narrator");
    EXPECT_EQ(
        workspace.find_character("guide")->character.appearance.weight,
        CharacterWeight::bold);
    ASSERT_NE(workspace.find_forum("lobby"), nullptr);
    ASSERT_EQ(workspace.find_forum("lobby")->members.size(), 1U);
    ASSERT_NE(workspace.find_forum_member("lobby", "guide"), nullptr);
    EXPECT_EQ(
        workspace.find_forum("lobby")->members.front().character_prompt,
        "Welcome, I am Guide in The Lobby.\n"
        "<character_profile>\nIntrinsic Guide.\n</character_profile>\n");
    EXPECT_EQ(workspace.find_character("guide")->markdown, "Intrinsic Guide.");
    ASSERT_NE(workspace.find_persona(workspace_guest_id), nullptr);
    EXPECT_EQ(
        workspace.find_persona(workspace_guest_id)->display_name,
        "Guest");
    ASSERT_NE(workspace.find_character(workspace_assistant_id), nullptr);
    EXPECT_EQ(
        workspace.find_character(workspace_assistant_id)->provider_id,
        "test");
    EXPECT_EQ(
        workspace.find_character(workspace_assistant_id)->voice_id,
        "warm-narrator");
    EXPECT_FALSE(
        workspace.find_character(workspace_assistant_id)->markdown.empty());
    ASSERT_NE(workspace.find_forum(workspace_entrance_id), nullptr);
    EXPECT_EQ(
        workspace.find_forum(workspace_entrance_id)->default_character_id,
        workspace_assistant_id);
    ASSERT_EQ(
        workspace.find_forum(workspace_entrance_id)->members.size(), 1U);
    EXPECT_FALSE(
        workspace.find_forum(workspace_entrance_id)
            ->members.front().system_prompt.empty());

    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "changed on disk\n";
    EXPECT_EQ(
        workspace.find_character("guide")->prompt_template,
        "$${greeting}, I am $${character.display_name} in "
        "$${forum.display_name}.\n"
        "<character_profile>\nIntrinsic Guide.\n</character_profile>\n");
}

TEST(Workspace, LoadsAMinimalVoice) {
    test::TestWorkspace fixture;
    fixture.write_voice(
        "plain-reader",
        "elevenlabs_voice_id = \"plain-voice-id\"\n");

    const Workspace workspace = Workspace::load(fixture.root());

    const WorkspaceVoice* const voice = workspace.find_voice("plain-reader");
    ASSERT_NE(voice, nullptr);
    EXPECT_EQ(voice->label, "Plain reader");
    EXPECT_TRUE(voice->description.empty());
    EXPECT_EQ(voice->elevenlabs_voice_id, "plain-voice-id");
    EXPECT_EQ(voice->settings, VoiceSettings{});

    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice(
            "plain-reader", "Plain reader", "", "plain-voice-id",
            VoiceSettings{.speed = 0.9});
    });
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.create_voice(
            "another-reader", "Another reader", "", "another-voice-id");
    });
    const Workspace reloaded = Workspace::load(fixture.root());
    ASSERT_NE(reloaded.find_voice("plain-reader"), nullptr);
    EXPECT_TRUE(reloaded.find_voice("plain-reader")->description.empty());
    EXPECT_EQ(reloaded.find_voice("plain-reader")->settings.speed, 0.9);
    ASSERT_NE(reloaded.find_voice("another-reader"), nullptr);
    EXPECT_TRUE(reloaded.find_voice("another-reader")->description.empty());
}

TEST(Workspace, RejectsInvalidVoiceConfigurationAndReferences) {
    {
        test::TestWorkspace fixture;
        fixture.write_voice(
            "too-fast",
            "elevenlabs_voice_id = \"voice-id\"\nspeed = 1.3\n");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        fixture.write_character_config(
            "display_name = \"Guide\"\n"
            "provider = \"test\"\n"
            "voice = \"missing\"\n");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
}

TEST(Workspace, IgnoresObsoleteVoiceSettingsAndDropsThemOnSave) {
    test::TestWorkspace fixture;
    fixture.write_voice(
        "reader",
        "elevenlabs_voice_id = \"fish-reference\"\n"
        "stability = \"steady\"\n"
        "similarity_boost = 9\n"
        "style = false\n"
        "use_speaker_boost = \"obsolete\"\n"
        "speed = 0.95\n");
    const auto log_file = fixture.root() / "voice-warnings.log";
    initialize_diagnostic_logging(log_file, "warn");
    const Workspace workspace = Workspace::load(fixture.root());
    const auto first_warnings = file_bytes(log_file);
    for (int i = 0; i < 3; ++i) {
        (void)Workspace::load(fixture.root());
    }
    shutdown_diagnostic_logging();
    EXPECT_EQ(file_bytes(log_file), first_warnings);
    const WorkspaceVoice* const voice = workspace.find_voice("reader");
    ASSERT_NE(voice, nullptr);
    EXPECT_EQ(voice->elevenlabs_voice_id, "fish-reference");
    EXPECT_EQ(voice->settings.speed, 0.95);
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice("reader", voice->label, voice->description,
            voice->elevenlabs_voice_id, voice->settings);
    });
    const auto saved = file_bytes(fixture.root() / "system" / "voices" / "reader" / "config.toml");
    for (const auto obsolete : {"stability", "similarity_boost", "style", "use_speaker_boost"}) {
        EXPECT_EQ(saved.find(obsolete), std::string::npos);
    }
    EXPECT_EQ(Workspace::load(fixture.root()).find_voice("reader")->settings.speed, 0.95);
}

TEST(Workspace, IgnoresElevenLabsOutputConfigurationAndRejectsSavingIt) {
    test::TestWorkspace fixture;
    const auto directory = fixture.root() / "system" / "voice-output";
    std::filesystem::create_directories(directory);
    const auto path = directory / "config.toml";
    std::ofstream(path)
        << "url = \"https://api.elevenlabs.io/v1/text-to-speech\"\n"
           "model = \"eleven_multilingual_v2\"\n"
           "api_key = \"api_key_2\"\n"
           "output_format = \"mp3_44100_128\"\n"
           "default_voice = \"Reader\"\n";
    const auto before = file_bytes(path);
    const Workspace workspace = Workspace::load(fixture.root());
    EXPECT_FALSE(workspace.voice_output());
    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_output({
            .url = "https://api.elevenlabs.io/v1/text-to-speech",
            .model = "eleven_multilingual_v2", .api_key_id = "api_key_2",
            .output_format = "mp3_44100_128", .default_voice = "Reader",
        });
    }), std::invalid_argument);
    EXPECT_EQ(file_bytes(path), before);
}

TEST(Workspace, CreatesUpdatesAssignsAndDeletesVoices) {
    test::TestWorkspace fixture;
    std::filesystem::create_directories(
        fixture.root() / "system" / "voices");
    const Workspace initial = Workspace::load(fixture.root());
    edit_fixture(initial, [&](WorkspaceConfigEditor& editor) {
        editor.create_voice(
            "voice_1", "Brian", "Deep, resonant, comforting", "brian-id");
    });
    EXPECT_EQ(initial.find_voice("voice_1"), nullptr);

    const Workspace created = Workspace::load(fixture.root());
    const WorkspaceVoice* voice = created.find_voice("voice_1");
    ASSERT_NE(voice, nullptr);
    EXPECT_EQ(voice->label, "Brian");
    EXPECT_EQ(voice->description, "Deep, resonant, comforting");
    EXPECT_TRUE(created.voice_is_writable("voice_1"));

    edit_fixture(created, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice(
            "voice_1", "George", "Warm, captivating storyteller", "george-id",
            VoiceSettings{
                .speed = 0.9,
            });
    });
    const Workspace updated = Workspace::load(fixture.root());
    voice = updated.find_voice("voice_1");
    ASSERT_NE(voice, nullptr);
    EXPECT_EQ(voice->label, "George");
    EXPECT_EQ(voice->elevenlabs_voice_id, "george-id");
    EXPECT_EQ(voice->settings.speed, 0.9);

    edit_fixture(updated, [&](WorkspaceConfigEditor& editor) {
        editor.write_character_settings(
            "guide", "test", std::nullopt, std::string_view{"voice_1"});
    });
    const Workspace assigned = Workspace::load(fixture.root());
    EXPECT_EQ(assigned.find_character("guide")->voice_id, "voice_1");
    EXPECT_THROW(edit_fixture(assigned, [&](WorkspaceConfigEditor& editor) {
        editor.delete_voice("voice_1");
    }), std::invalid_argument);

    edit_fixture(assigned, [&](WorkspaceConfigEditor& editor) {
        editor.write_character_settings(
            "guide", "test", std::nullopt, std::nullopt);
    });
    const Workspace cleared = Workspace::load(fixture.root());
    edit_fixture(cleared, [&](WorkspaceConfigEditor& editor) {
        editor.delete_voice("voice_1");
    });
    EXPECT_EQ(
        Workspace::load(fixture.root()).find_voice("voice_1"), nullptr);
}

TEST(Workspace, OmitsAnInvalidUnusedProvider) {
    test::TestWorkspace fixture;
    fixture.write_provider("unused", "host = \"localhost\"\nport = 80\n");

    const Workspace workspace = Workspace::load(fixture.root());
    EXPECT_EQ(workspace.find_provider("unused"), nullptr);
}

TEST(Workspace, WebSearchSettingsLoadDefaultsAndIgnoreInvalidFiles) {
    test::TestWorkspace fixture;
    const auto defaults = Workspace::load(fixture.root()).web_search();
    EXPECT_FALSE(defaults.enabled);
    EXPECT_EQ(defaults.provider, "brave");

    const auto path = fixture.root() / "system" / "web-search" / "config.toml";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "enabled = true\nprovider = 'tavily'\n"
                           "api_key = 'search-key'\nquery_provider = 'test'\n";
    const auto configured = Workspace::load(fixture.root()).web_search();
    EXPECT_TRUE(configured.enabled);
    EXPECT_EQ(configured.provider, "tavily");
    EXPECT_EQ(configured.api_key_id, "search-key");
    EXPECT_EQ(configured.query_provider_id, "test");

    std::ofstream(path) << "enabled = true\nprovider = 'google'\n";
    const auto unsupported = Workspace::load(fixture.root()).web_search();
    EXPECT_FALSE(unsupported.enabled);
    EXPECT_EQ(unsupported.provider, "brave");

    std::ofstream(path) << "enabled = [\n";
    const auto broken = Workspace::load(fixture.root()).web_search();
    EXPECT_FALSE(broken.enabled);
    EXPECT_EQ(broken.provider, "brave");
}

TEST(Workspace, ActiveSearchApiPreventsDeletingItsQueryProvider) {
    test::TestWorkspace fixture;
    fixture.write_provider("query", "host = 'test'\nport = 1\nmode = 'test'\nmodel = 'fake'\n");
    const auto path = fixture.root() / "system" / "web-search" / "config.toml";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "enabled = true\nprovider = 'brave'\n"
                           "api_key = 'search-key'\nquery_provider = 'query'\n";
    const Workspace active = Workspace::load(fixture.root());
    ASSERT_NE(active.find_provider("query"), nullptr);
    EXPECT_THROW(edit_fixture(active, [&](WorkspaceConfigEditor& editor) {
        editor.delete_provider("query");
    }), std::invalid_argument);

    std::ofstream(path) << "enabled = false\nprovider = 'brave'\n"
                           "api_key = 'search-key'\nquery_provider = 'query'\n";
    const Workspace disabled = Workspace::load(fixture.root());
    edit_fixture(disabled, [&](WorkspaceConfigEditor& editor) {
        editor.delete_provider("query");
    });
    EXPECT_EQ(Workspace::load(fixture.root()).find_provider("query"), nullptr);
}

TEST(Workspace, MissingAndEmptyProviderStringsHaveTheSameDiagnostics) {
    for (const std::string field : {"host", "model"}) {
        for (const bool missing : {false, true}) {
            SCOPED_TRACE(field);
            SCOPED_TRACE(missing);
            test::TestWorkspace fixture;
            const std::string other = field == "host"
                ? "model = \"fake\"\n" : "host = \"localhost\"\n";
            fixture.write_provider(
                "broken", "port = 80\n" + other
                    + (missing ? "" : field + " = \"\"\n"));
            fixture.write_character_config(
                "display_name = \"Guide\"\nprovider = \"broken\"\n");
            const std::string expected =
                "Character 'guide' references invalid provider 'broken': Provider config '"
                + (fixture.root() / "system" / "providers" / "broken"
                    / "config.toml").string()
                + "' requires non-empty string '" + field + "'";
            try {
                (void)Workspace::load(fixture.root());
                FAIL() << "Expected an invalid provider reference";
            } catch (const std::runtime_error& error) {
                EXPECT_EQ(error.what(), expected);
            }
        }
    }
}

TEST(Workspace, CharacterAndAssistantReferenceErrorsKeepTheirSubjectsAndOrder) {
    struct ReferenceCase {
        std::string settings;
        std::string error;
    };
    const ReferenceCase cases[]{
        {"provider = \"missing\"\nstyle = \"missing\"\nvoice = \"missing\"\n",
         " references unknown provider 'missing'"},
        {"provider = \"broken\"\nstyle = \"missing\"\nvoice = \"missing\"\n",
         " references invalid provider 'broken': "},
        {"provider = \"chat\"\nweb_search = \"auto\"\nstyle = \"missing\"\n",
         " enables web search for an unsupported provider"},
        {"provider = \"test\"\nstyle = \"missing\"\nvoice = \"missing\"\n",
         " references unknown style 'missing'"},
        {"provider = \"test\"\nvoice = \"missing\"\n",
         " references unknown voice 'missing'"},
    };
    for (const bool assistant : {false, true}) {
        for (const auto& [settings, error] : cases) {
            SCOPED_TRACE(settings);
            SCOPED_TRACE(assistant);
            test::TestWorkspace fixture;
            fixture.write_provider(
                "broken", "host = \"localhost\"\nport = 0\nmodel = \"fake\"\n");
            fixture.write_provider(
                "chat", "host = \"localhost\"\nport = 80\nmodel = \"fake\"\n"
                        "api = \"chat_completions\"\n");
            const std::string subject = assistant ? "Assistant" : "Character 'guide'";
            const auto path = assistant
                ? fixture.root() / "system" / "assistant" / "character.toml"
                : fixture.root() / "characters" / "guide" / "character.toml";
            std::ofstream(path)
                << "display_name = \"" << (assistant ? "Assistant" : "Guide")
                << "\"\n" << settings;
            std::string expected = subject + error;
            if (settings.starts_with("provider = \"broken\"")) {
                expected += "Provider config '"
                    + (fixture.root() / "system" / "providers" / "broken"
                       / "config.toml").string()
                    + "' requires port between 1 and 65535";
            }
            try {
                (void)Workspace::load(fixture.root());
                FAIL() << "Expected a broken reference";
            } catch (const std::runtime_error& failure) {
                EXPECT_EQ(failure.what(), expected);
            }
        }
    }
}

TEST(Workspace, ProviderlessDraftRejectsExplicitWebSearchOff) {
    test::TestWorkspace fixture;
    fixture.add_character("draft", "Draft");
    std::ofstream(fixture.root() / "characters" / "draft" / "character.toml")
        << "display_name = \"Draft\"\nweb_search = \"off\"\nstyle = \"missing\"\n";
    try {
        (void)Workspace::load(fixture.root());
        FAIL() << "Expected web search to require a provider";
    } catch (const std::runtime_error& failure) {
        EXPECT_STREQ(
            failure.what(), "Character 'draft' enables web search without a provider");
    }
}

TEST(Workspace, ExplicitWebSearchOffDoesNotRequireProviderCapability) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "chat", "host = \"localhost\"\nport = 80\nmodel = \"fake\"\n"
                "api = \"chat_completions\"\n");
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"chat\"\nweb_search = \"off\"\n");
    std::ofstream(fixture.root() / "system" / "assistant" / "character.toml")
        << "display_name = \"Assistant\"\nprovider = \"chat\"\nweb_search = \"off\"\n";

    const Workspace workspace = Workspace::load(fixture.root());
    EXPECT_EQ(workspace.find_character("guide")->web_search, WebSearchMode::off);
    EXPECT_EQ(
        workspace.find_character(workspace_assistant_id)->web_search, WebSearchMode::off);
}

TEST(Workspace, ResolvesCompleteProviderAndStyleValues) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "complete-provider",
        "host = \"provider.test\"\n"
        "port = 8443\n"
        "base_path = \"/api\"\n"
        "mode = \"net\"\n"
        "model = \"model-one\"\n"
        "api_key = \"api_key_1\"\n"
        "stream = false\n"
        "temperature = 0.25\n"
        "max_tokens = 512\n"
        "timeout_s = 90\n"
        "idle_timeout_s = 15\n"
        "reasoning_effort = \"high\"\n"
        "reasoning_format = \"reasoning\"\n"
        "https = true\n"
        "api = \"responses\"\n"
        "web_search = \"auto\"\n"
        "cache_retention = \"long\"\n");
    fixture.write_style(
        "loud-style",
        "font = \"mono\"\nstyle = \"italic\"\nweight = \"bold\"\n"
        "size = \"large\"\ntext_color = \"accent\"\n");
    fixture.write_character_config(
        "display_name = \"Guide\"\n"
        "provider = \"complete-provider\"\n"
        "style = \"loud-style\"\n"
        "reasoning_effort = \"xhigh\"\n"
        "web_search = \"required\"\n");

    const Workspace workspace = Workspace::load(fixture.root());
    const WorkspaceProvider* const provider =
        workspace.find_provider("complete-provider");
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->label, "Complete provider");
    EXPECT_EQ(provider->config.host, "provider.test");
    EXPECT_EQ(provider->config.port, 8443);
    EXPECT_EQ(provider->config.base_path, "/api");
    EXPECT_EQ(provider->config.mode, Mode::net);
    EXPECT_EQ(provider->config.model, "model-one");
    EXPECT_EQ(provider->config.api_key_id, "api_key_1");
    EXPECT_EQ(provider->config.auth, ProviderAuth::none);
    EXPECT_FALSE(provider->config.stream);
    EXPECT_EQ(provider->config.temperature, 0.25);
    EXPECT_EQ(provider->config.max_tokens, 512);
    EXPECT_EQ(provider->config.reasoning_format, ReasoningFormat::reasoning);
    EXPECT_EQ(provider->config.web_search, WebSearchMode::automatic);
    EXPECT_EQ(provider->config.cache_retention, CacheRetention::long_);
    EXPECT_TRUE(provider->config.openrouter_targets.empty());

    const WorkspaceCharacter* const character = workspace.find_character("guide");
    ASSERT_NE(character, nullptr);
    EXPECT_EQ(character->reasoning_effort, "xhigh");
    EXPECT_EQ(character->web_search, WebSearchMode::required);

    const WorkspaceStyle* const style = workspace.find_style("loud-style");
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->label, "Loud style");
    EXPECT_EQ(style->appearance.font, CharacterFont::mono);
    EXPECT_EQ(style->appearance.style, CharacterSlant::italic);
    EXPECT_EQ(style->appearance.weight, CharacterWeight::bold);
    EXPECT_EQ(style->appearance.size, CharacterScale::large);
    EXPECT_EQ(style->appearance.text_color, CharacterTextColor::accent);

    const CharacterDefinition definition =
        workspace.character_definition("lobby", "guide");
    const ModelBackendConfig& runtime = definition.provider.config;
    EXPECT_EQ(runtime.model, "model-one");
    EXPECT_EQ(runtime.api, ProviderApi::responses);
    EXPECT_EQ(runtime.reasoning_effort, "xhigh");
    EXPECT_EQ(runtime.web_search, WebSearchMode::required);
    EXPECT_EQ(provider->config.reasoning_effort, "high");
    EXPECT_EQ(provider->config.web_search, WebSearchMode::automatic);
}

TEST(Workspace, LoadsAndWritesOpenRouterTargets) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "router",
        "host = \"openrouter.ai\"\n"
        "port = 443\n"
        "https = true\n"
        "model = \"moonshotai/kimi-k2.6\"\n"
        "openrouter_targets = [\"CoreWeave\", \"Crusoe\"]\n");

    const Workspace workspace = Workspace::load(fixture.root());
    const WorkspaceProvider* provider = workspace.find_provider("router");
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(
        provider->config.openrouter_targets,
        (std::vector<std::string>{"CoreWeave", "Crusoe"}));

    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_provider("router", provider->label, provider->config);
    });
    const Workspace reloaded = Workspace::load(fixture.root());
    ASSERT_NE(reloaded.find_provider("router"), nullptr);
    EXPECT_EQ(
        reloaded.find_provider("router")->config.openrouter_targets,
        (std::vector<std::string>{"CoreWeave", "Crusoe"}));
}

TEST(Workspace, RejectsInvalidOpenRouterTargets) {
    ModelBackendConfig config;
    config.host = "openrouter.ai";
    config.openrouter_targets = {"CoreWeave", "coreweave"};
    EXPECT_FALSE(valid_openrouter_targets(config));

    config.openrouter_targets = {"Core Weave"};
    EXPECT_FALSE(valid_openrouter_targets(config));

    config.host = "api.openai.com";
    config.openrouter_targets = {"CoreWeave"};
    EXPECT_FALSE(valid_openrouter_targets(config));
}

TEST(Workspace, AllowsCharacterChatWebSearchOnlyForOpenRouter) {
    {
        test::TestWorkspace fixture;
        fixture.write_provider(
            "search",
            "host = \"OPENROUTER.AI.\"\n"
            "port = 443\n"
            "mode = \"test\"\n"
            "model = \"test-model\"\n"
            "api = \"chat_completions\"\n"
            "web_search = \"off\"\n");
        fixture.write_character_config(
            "display_name = \"Guide\"\n"
            "provider = \"search\"\n"
            "web_search = \"auto\"\n");

        const Workspace workspace = Workspace::load(fixture.root());
        ASSERT_NE(workspace.find_provider("search"), nullptr);
        EXPECT_EQ(
            workspace.find_provider("search")->config.web_search,
            WebSearchMode::off);
        EXPECT_EQ(
            workspace.character_definition("lobby", "guide")
                .provider.config.web_search,
            WebSearchMode::automatic);
    }
    {
        test::TestWorkspace fixture;
        fixture.write_provider(
            "search",
            "host = \"example.test\"\n"
            "port = 443\n"
            "mode = \"test\"\n"
            "model = \"test-model\"\n"
            "api = \"chat_completions\"\n"
            "web_search = \"off\"\n");
        fixture.write_character_config(
            "display_name = \"Guide\"\n"
            "provider = \"search\"\n"
            "web_search = \"auto\"\n");

        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
}

TEST(Workspace, ResolvesForumMemberOverridesAndDefaultPersonaPrompt) {
    test::TestWorkspace fixture;
    fixture.add_persona("author", "Author", "AUTHOR_ONLY_BODY");
    fixture.add_persona("reader", "Reader", "READER_ONLY_BODY");
    std::ofstream(fixture.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\n"
           "default_persona = \"reader\"\n";
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"test\"\n"
        "[prompt]\nvoice = \"definition\"\n");
    fixture.write_character_defaults("[prompt]\nvoice = \"default\"\n");
    std::ofstream(
        fixture.root() / "forums" / "lobby" / "members" / "guide"
            / "character.toml")
        << "[prompt]\nvoice = \"member\"\n";
    std::ofstream(
        fixture.root() / "forums" / "lobby" / "members" / "guide"
            / "CHARACTER.md",
        std::ios::binary)
        << "Member prompt: $${voice}\n";

    const Workspace workspace = Workspace::load(fixture.root());
    const WorkspaceForumMember* const member =
        workspace.find_forum_member("lobby", "guide");
    ASSERT_NE(member, nullptr);
    EXPECT_EQ(member->character_prompt, "Member prompt: member\n");
    EXPECT_NE(member->system_prompt.find("READER_ONLY_BODY"), std::string::npos);
    EXPECT_EQ(member->system_prompt.find("AUTHOR_ONLY_BODY"), std::string::npos);
    EXPECT_NE(
        member->system_prompt.find(shared_history_heading),
        std::string::npos);
    EXPECT_NE(
        member->system_prompt.find("earlier forum conversation context"),
        std::string::npos);
    EXPECT_NE(
        member->system_prompt.find("the current message you should answer"),
        std::string::npos);
}

TEST(Workspace, LoadsNestedDefinitionsAndKeepsCatalogsOrdered) {
    test::TestWorkspace fixture;
    const std::filesystem::path character =
        fixture.root() / "characters" / "group" / "alpha";
    std::filesystem::create_directories(character);
    std::ofstream(character / "character.toml")
        << "display_name = \"Alpha\"\nprovider = \"test\"\n";
    std::ofstream(character / "CHARACTER.md") << "Alpha instructions\n";
    const std::filesystem::path persona =
        fixture.root() / "personas" / "group" / "author";
    std::filesystem::create_directories(persona);
    std::ofstream(persona / "persona.toml")
        << "display_name = \"An Author\"\n";

    const Workspace workspace = Workspace::load(fixture.root());
    ASSERT_NE(workspace.find_character("alpha"), nullptr);
    ASSERT_NE(workspace.find_persona("author"), nullptr);
    ASSERT_GE(workspace.characters().size(), 3U);
    EXPECT_EQ(workspace.characters().front().character.display_name, "Alpha");
    ASSERT_GE(workspace.personas().size(), 3U);
    EXPECT_EQ(workspace.personas().front().display_name, "An Author");
    EXPECT_EQ(workspace.forums().front().display_name, "Entrance");
}

TEST(Workspace, EntranceInventoriesOrdinaryForumsBeforeFinalSortingAndIndexing) {
    test::TestWorkspace fixture;
    fixture.add_character("alpha", "Alpha");
    fixture.add_forum("early", "A Forum", "alpha");
    const Workspace workspace = Workspace::load(fixture.root());

    ASSERT_EQ(workspace.forums().size(), 3U);
    EXPECT_EQ(workspace.forums()[0].id, "early");
    EXPECT_EQ(workspace.forums()[1].id, workspace_entrance_id);
    EXPECT_EQ(workspace.forums()[2].id, "lobby");
    EXPECT_EQ(workspace.find_forum("early"), &workspace.forums()[0]);
    EXPECT_EQ(workspace.find_forum(workspace_entrance_id), &workspace.forums()[1]);
    EXPECT_EQ(workspace.find_forum("lobby"), &workspace.forums()[2]);
    const auto& prompt = workspace.forums()[1].members.front().system_prompt;
    EXPECT_NE(prompt.find("\"name\":\"A Forum\""), std::string::npos);
    EXPECT_NE(prompt.find("\"name\":\"The Lobby\""), std::string::npos);
    EXPECT_NE(prompt.find("\"default_character\":\"Alpha\""), std::string::npos);
    EXPECT_NE(prompt.find("\"default_persona\":\"Guest\""), std::string::npos);
    EXPECT_EQ(prompt.find("\"name\":\"Entrance\""), std::string::npos);
}

TEST(Workspace, AllowsProviderlessDraftOutsideForumsOnly) {
    test::TestWorkspace fixture;
    const std::filesystem::path draft =
        fixture.root() / "characters" / "draft";
    std::filesystem::create_directories(draft);
    std::ofstream(draft / "character.toml")
        << "display_name = \"Draft\"\n"
           "description = \"Not configured yet.\"\n";
    std::ofstream(draft / "CHARACTER.md") << "Draft profile\n";

    const Workspace workspace = Workspace::load(fixture.root());
    const WorkspaceCharacter* character = workspace.find_character("draft");
    ASSERT_NE(character, nullptr);
    EXPECT_FALSE(character->provider_id);

    std::filesystem::create_directories(
        fixture.root() / "forums" / "lobby" / "members" / "draft");
    EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
}

TEST(Workspace, NewCharacterPreservesAnExistingSharedVoice) {
    test::TestWorkspace fixture;
    const std::filesystem::path shared_voice =
        fixture.root() / "characters" / "character-voice.md";
    std::ofstream(shared_voice, std::ios::binary)
        << "Customized shared voice.\n";

    const Workspace workspace = Workspace::load(fixture.root());
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.create_character("newcomer", "Newcomer", "A new character.");
    });

    EXPECT_EQ(file_bytes(shared_voice), "Customized shared voice.\n");
    EXPECT_EQ(
        file_bytes(
            fixture.root() / "characters" / "newcomer" / "CHARACTER.md"),
        "$$(../character-voice.md)\n\n"
        "<character_profile>\n$$(PROFILE.md)\n</character_profile>\n");
    const Workspace reloaded = Workspace::load(fixture.root());
    ASSERT_NE(reloaded.find_character("newcomer"), nullptr);
    EXPECT_TRUE(reloaded.find_character("newcomer")->markdown.empty());
}

TEST(Workspace, RejectsBrokenReferencesAndIdentityCollisions) {
    {
        test::TestWorkspace fixture;
        fixture.write_character_config(
            "display_name = \"Guide\"\nprovider = \"missing\"\n");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        fixture.write_character_config(
            "display_name = \"Guide\"\nprovider = \"test\"\n"
            "style = \"missing\"\n");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        fixture.add_character("duplicate", "Guide");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        fixture.add_persona("guide", "Writer");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        std::ofstream(fixture.root() / "forums" / "lobby" / "config.toml")
            << "display_name = \"The Lobby\"\n"
               "default_character = \"missing\"\n";
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
}

TEST(Workspace, RequiresTheWorkspaceDirectoryStructureAndDefinitionFiles) {
    {
        test::TestWorkspace fixture;
        std::filesystem::remove_all(fixture.root() / "personas");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        std::filesystem::remove(
            fixture.root() / "characters" / "guide" / "CHARACTER.md");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        std::filesystem::remove(
            fixture.root() / "forums" / "lobby" / "FORUM.md");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
}

TEST(Workspace, TemplateIncludesResolveUnderThePhysicalRoot) {
    test::TestWorkspace fixture;
    std::ofstream(fixture.root() / "characters" / "guide" / "voice.md")
        << "Physical include body.\n";
    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "$$(voice.md)\nGuide instructions.\n";
    const Workspace workspace = Workspace::load(fixture.root());
    ASSERT_NE(workspace.find_character("guide"), nullptr);
    EXPECT_NE(
        workspace.find_character("guide")->markdown.find(
            "Physical include body."),
        std::string::npos);
}

TEST(Workspace, CharacterFileEditsRequireAnExactListedFilename) {
    test::TestWorkspace fixture;
    const auto directory = fixture.root() / "characters" / "guide";
    std::ofstream(directory / "PROFILE.md", std::ios::binary) << "Original profile\n";
    std::ofstream(fixture.root() / "forums" / "lobby" / "NOTES.md") << "Forum notes\n";
    const Workspace workspace = Workspace::load(fixture.root());
    const auto original = file_bytes(directory / "CHARACTER.md");
    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_character_file("guide", "profile.md", "Changed");
    }), std::out_of_range);
    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_character_file("guide", "character.md", std::nullopt);
    }), std::out_of_range);
    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_character_file("guide", "CHARACTER.md", std::nullopt);
    }), std::invalid_argument);
    EXPECT_EQ(file_bytes(directory / "PROFILE.md"), "Original profile\n");
    EXPECT_EQ(file_bytes(directory / "CHARACTER.md"), original);
    // A listed file can disappear after the immutable workspace snapshot was loaded.
    std::filesystem::remove(directory / "PROFILE.md");
    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_character_file("guide", "PROFILE.md", "Changed");
    }), std::out_of_range);
    const auto forum_file = fixture.root() / "forums" / "lobby" / "NOTES.md";
    std::filesystem::remove(forum_file);
    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_forum_file("lobby", "NOTES.md", "Changed");
    }), std::out_of_range);
    EXPECT_FALSE(std::filesystem::exists(directory / "PROFILE.md"));
    EXPECT_FALSE(std::filesystem::exists(forum_file));
}

TEST(Workspace, CharacterVoiceMatchesLegacyIncludesAfterMovingTheCharacter) {
    test::TestWorkspace fixture;
    const auto characters = fixture.root() / "characters";
    std::ofstream(characters / "character-voice.md", std::ios::binary)
        << "Portray $${character.display_name} in $${forum.display_name}.\n";
    std::ofstream(characters / "guide" / "PROFILE.md", std::ios::binary) << "Guide profile.\n";
    std::ofstream(characters / "guide" / "CHARACTER.md", std::ios::binary)
        << "$$(../character-voice.md)\n<character_profile>\n"
           "$$(PROFILE.md)</character_profile>\n";
    const Workspace legacy = Workspace::load(fixture.root());
    const std::string expected = legacy.find_forum_member("lobby", "guide")->character_prompt;

    std::ofstream(characters / "guide" / "CHARACTER.md", std::ios::binary)
        << "$${CHARACTER_VOICE}\n<character_profile>\n"
           "$$(PROFILE.md)</character_profile>\n";
    std::filesystem::create_directories(characters / "group" / "nested");
    std::filesystem::rename(characters / "guide", characters / "group" / "nested" / "guide");
    const Workspace moved = Workspace::load(fixture.root());
    ASSERT_NE(moved.find_character("guide"), nullptr);
    EXPECT_EQ(moved.find_character("guide")->markdown, legacy.find_character("guide")->markdown);
    EXPECT_EQ(moved.find_forum_member("lobby", "guide")->character_prompt, expected);

    // Forum member templates can use the same shared file and their own includes.
    const auto member = fixture.root() / "forums" / "lobby" / "members" / "guide";
    std::ofstream(member / "PROFILE.md", std::ios::binary) << "Override profile.\n";
    std::ofstream(member / "CHARACTER.md", std::ios::binary)
        << "$${CHARACTER_VOICE}\n$$(PROFILE.md)";
    const Workspace overridden = Workspace::load(fixture.root());
    EXPECT_EQ(
        overridden.find_forum_member("lobby", "guide")->character_prompt,
        "Portray Guide in The Lobby.\n\nOverride profile.\n");
}

TEST(Workspace, ForumDefinitionExpandsSharedAndLocalFilesForEachMember) {
    test::TestWorkspace fixture;
    const auto forums = fixture.root() / "forums";
    const auto directory = forums / "lobby";
    const Workspace plain = Workspace::load(fixture.root());
    EXPECT_FALSE(std::filesystem::exists(forums / "forum-definition.md"));
    edit_fixture(plain, [&](WorkspaceConfigEditor& editor) {
        editor.create_forum("newcomer", "Newcomer", "reader");
    });
    EXPECT_FALSE(std::filesystem::exists(forums / "forum-definition.md"));
    EXPECT_EQ(file_bytes(forums / "newcomer" / "FORUM.md"), "");

    std::ofstream(forums / "forum-definition.md")
        << "$$(shared.md)\n";
    std::ofstream(forums / "shared.md")
        << "$${character.display_name} in $${forum.display_name}.\n";
    std::ofstream(fixture.root() / "characters" / "character-voice.md")
        << "Voice of $${character.display_name}.\n";
    std::ofstream(directory / "HOUSE-RULES.md") << "Local rules.\n";
    std::ofstream(directory / "NOTES.md") << "$$(unused.md)";
    std::ofstream(directory / "FORUM.md", std::ios::binary)
        << "$${FORUM_DEFINITION}\n$${CHARACTER_VOICE}\n$$(HOUSE-RULES.md)";
    fixture.add_character("writer", "Writer");
    std::filesystem::create_directories(directory / "members" / "writer");
    std::ofstream(directory / "members" / "writer" / "character.toml") << "# Member\n";
    const Workspace workspace = Workspace::load(fixture.root());
    const auto* forum = workspace.find_forum("lobby");
    ASSERT_NE(forum, nullptr);
    EXPECT_EQ(forum->markdown_files.size(), 3U);
    EXPECT_FALSE(forum->markdown_files.contains("forum-definition.md"));
    EXPECT_EQ(forum->markdown_files.at("NOTES.md"), "$$(unused.md)");
    EXPECT_EQ(forum->prompt_template, "$${FORUM_DEFINITION}\n$${CHARACTER_VOICE}\n$$(HOUSE-RULES.md)");
    for (const auto& member : forum->members) {
        const auto* character = workspace.find_character(member.character_id);
        EXPECT_NE(member.system_prompt.find(
            character->character.display_name + " in The Lobby."), std::string::npos);
        EXPECT_NE(member.system_prompt.find("Local rules."), std::string::npos);
        EXPECT_NE(member.system_prompt.find("Voice of " + character->character.display_name + "."), std::string::npos);
        EXPECT_EQ(member.system_prompt.find("FORUM_DEFINITION"), std::string::npos);
    }
    std::filesystem::remove(forums / "forum-definition.md");
    EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    std::ofstream(directory / "FORUM.md") << "Plain text still works.\n";
    EXPECT_NO_THROW((void)Workspace::load(fixture.root()));
}

TEST(Workspace, LoadsLegacyProviderCredentialNamesWithoutUsingTheEnvironment) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "secured",
        "host = \"example.test\"\n"
        "port = 443\n"
        "mode = \"net\"\n"
        "model = \"secured\"\n"
        "api_key_env = \"OPENAI_API_KEY\"\n");
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"secured\"\n");

    const Workspace workspace = Workspace::load(fixture.root());
    const WorkspaceProvider* const provider = workspace.find_provider("secured");
    ASSERT_NE(provider, nullptr);
    EXPECT_TRUE(provider->config.api_key_id.empty());
    EXPECT_EQ(provider->config.api_key_env, "OPENAI_API_KEY");
}

TEST(Workspace, CreatesAProviderByCopyingExistingSettings) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "source",
        "display_name = \"Source\"\n"
        "host = \"example.test\"\n"
        "port = 8443\n"
        "base_path = \"/models\"\n"
        "mode = \"net\"\n"
        "model = \"source-model\"\n"
        "stream = false\n"
        "temperature = 0.25\n"
        "max_tokens = 321\n"
        "timeout_s = 42\n"
        "idle_timeout_s = 7\n"
        "api_key = \"api_key_1\"\n"
        "reasoning_effort = \"high\"\n"
        "reasoning_format = \"reasoning\"\n"
        "https = true\n"
        "api = \"chat_completions\"\n"
        "auth = \"none\"\n"
        "web_search = \"off\"\n"
        "cache_retention = \"long\"\n");

    const Workspace workspace = Workspace::load(fixture.root());
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.create_provider("copied", "Copied provider", "source");
    });

    const Workspace reloaded = Workspace::load(fixture.root());
    const WorkspaceProvider* const copied = reloaded.find_provider("copied");
    ASSERT_NE(copied, nullptr);
    EXPECT_EQ(copied->label, "Copied provider");
    EXPECT_EQ(copied->config.host, "example.test");
    EXPECT_EQ(copied->config.port, 8443);
    EXPECT_EQ(copied->config.base_path, "/models");
    EXPECT_EQ(copied->config.model, "source-model");
    EXPECT_FALSE(copied->config.stream);
    EXPECT_EQ(copied->config.temperature, 0.25);
    EXPECT_EQ(copied->config.max_tokens, 321);
    EXPECT_EQ(copied->config.timeout_s, 42);
    EXPECT_EQ(copied->config.idle_timeout_s, 7);
    EXPECT_EQ(copied->config.api_key_id, "api_key_1");
    EXPECT_EQ(copied->config.reasoning_effort, "high");
    EXPECT_EQ(copied->config.reasoning_format, ReasoningFormat::reasoning);
    EXPECT_EQ(copied->config.api, ProviderApi::chat_completions);
    EXPECT_EQ(copied->config.cache_retention, CacheRetention::long_);

    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.create_provider("bad_copy", "Bad copy", "missing");
        }),
        std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(
        fixture.root() / "system" / "providers" / "bad_copy"));
}

TEST(Workspace, WritesConfigurationWithoutChangingTheLoadedInstance) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "second",
        "host = \"test\"\nport = 2\nmode = \"test\"\nmodel = \"second\"\n");
    fixture.write_style("mono", "font = \"mono\"\n");
    fixture.add_character("writer", "Writer");
    std::filesystem::create_directories(
        fixture.root() / "forums" / "lobby" / "members" / "writer");

    const Workspace workspace = Workspace::load(fixture.root());
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_character_settings(
            "guide", "second", std::string_view{"mono"},
            std::nullopt, std::string_view{"xhigh"}, WebSearchMode::required);
    });
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_forum_default_character("lobby", "writer");
    });
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_forum_default_persona("lobby", "reader");
    });

    EXPECT_EQ(workspace.find_character("guide")->provider_id, "test");
    EXPECT_EQ(
        workspace.find_forum("lobby")->default_character_id,
        "guide");
    EXPECT_EQ(
        workspace.find_forum("lobby")->default_persona_id,
        workspace_guest_id);

    const Workspace reloaded = Workspace::load(fixture.root());
    EXPECT_EQ(reloaded.find_character("guide")->provider_id, "second");
    EXPECT_EQ(reloaded.find_character("guide")->style_id, "mono");
    EXPECT_EQ(reloaded.find_character("guide")->reasoning_effort, "xhigh");
    EXPECT_EQ(reloaded.find_character("guide")->web_search, WebSearchMode::required);
    EXPECT_EQ(
        reloaded.find_forum("lobby")->default_character_id,
        "writer");
    EXPECT_EQ(
        reloaded.find_forum("lobby")->default_persona_id,
        "reader");
}

TEST(Workspace, RejectsInvalidWritesWithoutChangingTheConfigFile) {
    test::TestWorkspace fixture;
    const Workspace workspace = Workspace::load(fixture.root());
    const std::filesystem::path character =
        fixture.root() / "characters" / "guide" / "character.toml";
    const std::string before = file_bytes(character);

    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_character_settings("guide", "missing", std::nullopt);
        }),
        std::invalid_argument);
    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_character_settings(
                "guide", "test", std::string_view{"missing"});
        }),
        std::invalid_argument);
    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_character_settings(
                "guide", "test", std::nullopt, std::nullopt,
                std::string_view{"extreme"});
        }),
        std::invalid_argument);
    EXPECT_EQ(file_bytes(character), before);

    const std::filesystem::path provider =
        fixture.root() / "system" / "providers" / "test" / "config.toml";
    const std::string provider_before = file_bytes(provider);
    const ModelBackendConfig original = workspace.find_provider("test")->config;
    ModelBackendConfig config = original;
    config.port = 0;
    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_provider("test", "Test", config);
        }), std::invalid_argument);
    config = original;
    config.api_key_id = "api_key_1";
    config.api_key_env = "Legacy Key";
    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_provider("test", "Test", config);
        }), std::invalid_argument);
    config = original;
    config.host.clear();
    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_provider("test", "Test", config);
        }), std::invalid_argument);
    EXPECT_EQ(file_bytes(provider), provider_before);
    config = original;
    config.model.clear();
    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_provider("test", "Test", config);
        }), std::invalid_argument);
    EXPECT_EQ(file_bytes(provider), provider_before);

    EXPECT_TRUE(workspace.character_is_writable("guide"));
    EXPECT_FALSE(workspace.character_is_writable(workspace_assistant_id));
    EXPECT_TRUE(workspace.character_settings_are_writable("guide"));
    EXPECT_TRUE(workspace.character_settings_are_writable(workspace_assistant_id));
    EXPECT_FALSE(workspace.character_settings_are_writable("missing"));
}

TEST(Workspace, WritesAssistantSettingsWithoutMakingItsDefinitionWritable) {
    test::TestWorkspace fixture;
    fixture.write_style("mono", "font = \"mono\"\n");
    const Workspace workspace = Workspace::load(fixture.root());

    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_character_settings(
            workspace_assistant_id, "test", std::string_view{"mono"},
            std::nullopt, std::string_view{"high"}, WebSearchMode::off);
    });

    const Workspace reloaded = Workspace::load(fixture.root());
    const WorkspaceCharacter* const assistant =
        reloaded.find_character(workspace_assistant_id);
    ASSERT_NE(assistant, nullptr);
    EXPECT_EQ(assistant->provider_id, "test");
    EXPECT_EQ(assistant->style_id, "mono");
    EXPECT_EQ(assistant->reasoning_effort, "high");
    EXPECT_EQ(assistant->web_search, WebSearchMode::off);
    EXPECT_TRUE(reloaded.character_settings_are_writable(workspace_assistant_id));
    EXPECT_FALSE(reloaded.character_is_writable(workspace_assistant_id));
}

TEST(Workspace, LoadingAnInvalidWorkspaceDoesNotChangeAnExistingSnapshot) {
    test::TestWorkspace valid;
    const Workspace published = Workspace::load(valid.root());
    test::TestWorkspace invalid;
    invalid.write_character_config("display_name =\n");
    EXPECT_THROW((void)Workspace::load(invalid.root()), std::runtime_error);
    EXPECT_EQ(published.root(), valid.root());
    EXPECT_NE(published.find_character("guide"), nullptr);
}

TEST(Workspace, ValidatesVoiceInputBeforeWritingAndIgnoresInvalidSavedConfig) {
    test::TestWorkspace fixture;
    const Workspace workspace = Workspace::load(fixture.root());
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_input({
            .url = "https://api.openai.com/v1/realtime/calls",
            .model = "gpt-live-transcribe",
            .api_key_id = "api_key_1",
            .delay = "high",
            .prompt = "Technical discussion.",
        });
    });
    const std::filesystem::path path =
        fixture.root() / "system" / "voice-input" / "config.toml";
    const std::string before = file_bytes(path);

    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_voice_input({
                .url = "not-a-url",
                .model = "gpt-live-transcribe",
                .api_key_id = "api_key_1",
            });
        }),
        std::invalid_argument);
    EXPECT_EQ(file_bytes(path), before);

    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_voice_input({
                .url = "https://api.openai.com/v1/realtime/calls",
                .model = "gpt-live-transcribe",
                .api_key_id = "api_key_1",
                .delay = "maximum",
            });
        }),
        std::invalid_argument);
    EXPECT_EQ(file_bytes(path), before);

    std::ofstream(path) << "url = \"https://api.openai.com/v1/realtime/calls\"\n"
                          "model = \"gpt-live-transcribe\"\n"
                          "api_key = \"api_key_1\"\n";
    const Workspace legacy = Workspace::load(fixture.root());
    ASSERT_TRUE(legacy.voice_input());
    EXPECT_EQ(legacy.voice_input()->provider, "openai");
    EXPECT_EQ(legacy.voice_input()->delay, "low");
    EXPECT_TRUE(legacy.voice_input()->prompt.empty());

    std::ofstream(path) << "url = \"not-a-url\"\n"
                          "model = \"gpt-live-transcribe\"\n"
                          "api_key = \"api_key_1\"\n";
    const Workspace reloaded = Workspace::load(fixture.root());
    EXPECT_FALSE(reloaded.voice_input());
}

TEST(Workspace, SelectsVoiceInputUrlsAndNormalizesUnusedXaiDelay) {
    test::TestWorkspace fixture;
    const auto directory = fixture.root() / "system" / "voice-input";
    std::filesystem::create_directories(directory);
    const auto path = directory / "config.toml";
    const auto log_file = fixture.root() / "voice-input-warnings.log";

    const auto write_config = [&](std::string_view body) {
        std::ofstream(path) << body;
    };
    const auto load_provider = [&]() -> std::optional<std::string> {
        const auto loaded = Workspace::load(fixture.root()).voice_input();
        if (!loaded) return std::nullopt;
        return loaded->provider;
    };

    write_config(
        "url = \"https://api.openai.com/v1/realtime/calls\"\n"
        "model = \"gpt-live-transcribe\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_EQ(load_provider(), "openai");

    write_config(
        "provider = \"openai\"\n"
        "url = \"http://127.0.0.1:9/v1/realtime\"\n"
        "model = \"gpt-live-transcribe\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_EQ(load_provider(), "openai");

    write_config(
        "provider = \"xai\"\n"
        "url = \"wss://api.x.ai/v1/stt\"\n"
        "model = \"grok-voice-transcribe-2.0\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_EQ(load_provider(), "xai");

    write_config(
        "provider = \"xai\"\n"
        "url = \"ws://127.0.0.1:9/v1/stt\"\n"
        "model = \"grok-voice-transcribe-2.0\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_EQ(load_provider(), "xai");

    initialize_diagnostic_logging(log_file, "warn");
    write_config(
        "url = \"wss://api.x.ai/v1/stt\"\n"
        "model = \"gpt-live-transcribe\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_FALSE(load_provider());
    write_config(
        "provider = \"xai\"\n"
        "url = \"https://api.openai.com/v1/realtime/calls\"\n"
        "model = \"grok-voice-transcribe-2.0\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_FALSE(load_provider());
    write_config(
        "provider = \"openai\"\n"
        "url = \"ftp://example.com/stt\"\n"
        "model = \"gpt-live-transcribe\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_FALSE(load_provider());
    write_config(
        "provider = \"xai\"\n"
        "url = \"wss://\"\n"
        "model = \"grok-voice-transcribe-2.0\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_FALSE(load_provider());
    write_config(
        "provider = \"openai\"\n"
        "url = \"https://bad host/stt\"\n"
        "model = \"gpt-live-transcribe\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_FALSE(load_provider());
    write_config(
        "provider = \"grok\"\n"
        "url = \"https://api.openai.com/v1/realtime/calls\"\n"
        "model = \"gpt-live-transcribe\"\n"
        "api_key = \"api_key_1\"\n");
    EXPECT_FALSE(load_provider());
    write_config(
        "provider = \"xai\"\n"
        "url = \"wss://api.x.ai/v1/stt\"\n"
        "model = \"grok-voice-transcribe-2.0\"\n"
        "api_key = \"api_key_1\"\n"
        "delay = \"obsolete-delay-value\"\n"
        "prompt = \"keep\"\n");
    const Workspace normalized = Workspace::load(fixture.root());
    ASSERT_TRUE(normalized.voice_input());
    EXPECT_EQ(normalized.voice_input()->provider, "xai");
    EXPECT_EQ(normalized.voice_input()->delay, "low");
    EXPECT_EQ(normalized.voice_input()->prompt, "keep");
    EXPECT_EQ(normalized.voice_input()->send_phrase, "over to you");
    shutdown_diagnostic_logging();
    const std::string warnings = file_bytes(log_file);
    EXPECT_NE(warnings.find(std::string(openai_voice_input_url_message)), std::string::npos);
    EXPECT_NE(warnings.find(std::string(xai_voice_input_url_message)), std::string::npos);
    EXPECT_NE(warnings.find("Ignoring obsolete voice input delay for xAI; using low"),
        std::string::npos);
    EXPECT_EQ(warnings.find("obsolete-delay-value"), std::string::npos);

    const Workspace workspace = Workspace::load(fixture.root());
    const auto saved_before = file_bytes(path);
    try {
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_voice_input({
                .provider = "openai",
                .url = "wss://api.x.ai/v1/stt",
                .model = "gpt-live-transcribe",
                .api_key_id = "api_key_1",
            });
        });
        FAIL() << "OpenAI must reject a WebSocket URL";
    } catch (const std::invalid_argument& error) {
        EXPECT_EQ(error.what(), std::string(openai_voice_input_url_message));
    }
    EXPECT_EQ(file_bytes(path), saved_before);

    try {
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_voice_input({
                .provider = "xai",
                .url = "https://api.openai.com/v1/realtime/calls",
                .model = "grok-voice-transcribe-2.0",
                .api_key_id = "api_key_1",
            });
        });
        FAIL() << "xAI must reject an HTTP URL";
    } catch (const std::invalid_argument& error) {
        EXPECT_EQ(error.what(), std::string(xai_voice_input_url_message));
    }
    EXPECT_EQ(file_bytes(path), saved_before);

    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_input({
            .provider = "xai",
            .url = "ws://127.0.0.1:9/v1/stt",
            .model = "grok-voice-transcribe-2.0",
            .api_key_id = "api_key_1",
            .delay = "obsolete-delay-value",
            .prompt = "keep",
        });
    });
    const Workspace saved = Workspace::load(fixture.root());
    ASSERT_TRUE(saved.voice_input());
    EXPECT_EQ(saved.voice_input()->provider, "xai");
    EXPECT_EQ(saved.voice_input()->url, "ws://127.0.0.1:9/v1/stt");
    EXPECT_EQ(saved.voice_input()->delay, "low");
    EXPECT_EQ(saved.voice_input()->prompt, "keep");
    EXPECT_EQ(file_bytes(path).find("obsolete-delay-value"), std::string::npos);

    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_input({
            .provider = "openai",
            .url = "https://api.openai.com/v1/realtime/calls",
            .model = "gpt-live-transcribe",
            .api_key_id = "api_key_1",
        });
    });
    edit_fixture(Workspace::load(fixture.root()), [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_input({
            .provider = "xai",
            .url = "wss://api.x.ai/v1/stt",
            .model = "grok-voice-transcribe-2.0",
            .api_key_id = "api_key_1",
        });
    });
    const Workspace defaults = Workspace::load(fixture.root());
    ASSERT_TRUE(defaults.voice_input());
    EXPECT_EQ(defaults.voice_input()->provider, "xai");
    EXPECT_EQ(defaults.voice_input()->url, "wss://api.x.ai/v1/stt");
    EXPECT_EQ(defaults.voice_input()->model, "grok-voice-transcribe-2.0");

    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_input({
            .provider = "openai",
            .url = "https://api.openai.com/v1/realtime/calls",
            .model = "gpt-live-transcribe",
            .api_key_id = "api_key_1",
            .delay = "obsolete-delay-value",
        });
    }), std::invalid_argument);
    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_input({
            .provider = "grok",
            .url = "https://api.openai.com/v1/realtime/calls",
            .model = "gpt-live-transcribe",
            .api_key_id = "api_key_1",
        });
    }), std::invalid_argument);
}

TEST(Workspace, NormalizesFishAudioConfigurationOnWriteAndLoad) {
    test::TestWorkspace fixture;
    const Workspace workspace = Workspace::load(fixture.root());
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_output({
            .url = "HTTPS://API.FISH.AUDIO:443", .model = " custom/model ",
            .api_key_id = "api_key_2", .output_format = "mp3", .default_voice = "Reader",
        });
    });
    auto reloaded = Workspace::load(fixture.root());
    ASSERT_TRUE(reloaded.voice_output());
    EXPECT_EQ(reloaded.voice_output()->url, "https://api.fish.audio/v1/tts");
    EXPECT_EQ(reloaded.voice_output()->model, "custom/model");
    const auto path = fixture.root() / "system" / "voice-output" / "config.toml";
    std::ofstream(path) << "url = \"HTTPS://API.FISH.AUDIO:443\"\n"
                          "model = \" s2.1-pro \"\n"
                          "api_key = \"api_key_2\"\noutput_format = \"mp3\"\ndefault_voice = \"Reader\"\n";
    reloaded = Workspace::load(fixture.root());
    ASSERT_TRUE(reloaded.voice_output());
    EXPECT_EQ(reloaded.voice_output()->url, "https://api.fish.audio/v1/tts");
    EXPECT_EQ(reloaded.voice_output()->model, "s2.1-pro");
    const std::string before = file_bytes(path);
    EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_output({
            .url = "http://api.fish.audio/v1/tts", .model = "s2.1-pro",
            .api_key_id = "api_key_2", .output_format = "mp3", .default_voice = "Reader",
        });
    }), std::invalid_argument);
    EXPECT_EQ(file_bytes(path), before);
}

TEST(Workspace, PersistsAndValidatesVoiceOutputSettings) {
    test::TestWorkspace fixture;
    fixture.write_voice(
        "default-reader",
        "display_name = \"Default Reader\"\n"
        "elevenlabs_voice_id = \"eleven-default\"\n");
    const Workspace workspace = Workspace::load(fixture.root());
    edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
        editor.write_voice_output({
            .url = "https://api.fish.audio/v1/tts",
            .model = "s2.1-pro",
            .api_key_id = "api_key_2",
            .output_format = "mp3_44100_128",
            .default_voice = "Default Reader",
        });
    });
    const std::filesystem::path path =
        fixture.root() / "system" / "voice-output" / "config.toml";
    const std::string before = file_bytes(path);

    const Workspace reloaded = Workspace::load(fixture.root());
    ASSERT_TRUE(reloaded.voice_output());
    EXPECT_EQ(reloaded.voice_output()->model, "s2.1-pro");
    EXPECT_EQ(reloaded.voice_output()->api_key_id, "api_key_2");
    EXPECT_EQ(reloaded.voice_output()->output_format, "mp3");
    EXPECT_EQ(reloaded.voice_output()->default_voice, "Default Reader");
    ASSERT_NE(reloaded.find_voice_by_name("Default Reader"), nullptr);
    EXPECT_EQ(
        reloaded.find_voice_by_name("Default Reader")->elevenlabs_voice_id,
        "eleven-default");

    EXPECT_THROW(
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_voice_output({
                .url = "not-a-url",
                .model = "s2.1-pro",
                .api_key_id = "api_key_2",
                .output_format = "mp3_44100_128",
                .default_voice = "Default Reader",
            });
        }),
        std::invalid_argument);
    EXPECT_EQ(file_bytes(path), before);

    std::ofstream(path) << "url = \"not-a-url\"\n"
                          "model = \"s2.1-pro\"\n"
                          "api_key = \"api_key_2\"\n"
                          "output_format = \"mp3_44100_128\"\n"
                          "default_voice = \"Default Reader\"\n";
    EXPECT_FALSE(Workspace::load(fixture.root()).voice_output());
}

TEST(Workspace, NormalizesLegacyVoiceOutputFormatsAndRejectsUnsupportedSaves) {
    test::TestWorkspace fixture;
    const Workspace workspace = Workspace::load(fixture.root());
    const auto path = fixture.root() / "system" / "voice-output" / "config.toml";
    for (const auto& [legacy, format] : {
             std::pair{"mp3_44100_128", "mp3"},
             std::pair{"opus_48000_64", "opus"},
             std::pair{" wav ", "wav"}}) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path) << "url = \"https://api.fish.audio/v1/tts\"\n"
                              "model = \"s2.1-pro\"\napi_key = \"api_key_2\"\n"
                              "default_voice = \"Reader\"\noutput_format = \"" << legacy << "\"\n";
        const Workspace loaded = Workspace::load(fixture.root());
        ASSERT_TRUE(loaded.voice_output());
        EXPECT_EQ(loaded.voice_output()->output_format, format);
        auto settings = *loaded.voice_output();
        settings.output_format = legacy;
        edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_voice_output(settings);
        });
        EXPECT_NE(file_bytes(path).find(std::string("output_format = '") + format + "'"), std::string::npos);
    }
    const auto before = file_bytes(path);
    auto settings = *Workspace::load(fixture.root()).voice_output();
    for (const auto invalid : {"pcm_44100", "flac", "", "mp3_bad", "opus_48000_"}) {
        settings.output_format = invalid;
        EXPECT_THROW(edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
            editor.write_voice_output(settings);
        }), std::invalid_argument);
        EXPECT_EQ(file_bytes(path), before);
    }
    std::ofstream(path) << "url = \"https://api.fish.audio/v1/tts\"\n"
                          "model = \"s2.1-pro\"\napi_key = \"api_key_2\"\n"
                          "default_voice = \"Reader\"\noutput_format = \"pcm_44100\"\n";
    const Workspace loaded = Workspace::load(fixture.root());
    ASSERT_TRUE(loaded.voice_output());
    EXPECT_EQ(loaded.voice_output()->output_format, "mp3");
}

TEST(Workspace, ResolvesForumCharacterHandles) {
    const Workspace workspace = workspace_with_characters({
        {"ada", "Ada"},
        {"grace", "Grace"},
    });

    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "ADA").character->id,
        "ada");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "Ada,").character->id,
        "ada");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "gr").character->id,
        "grace");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "?!").match,
        HandleMatch::unknown);
    EXPECT_EQ(workspace.forum_handle_list("lobby"), "@Ada, @Grace");
    ASSERT_NE(workspace.find_forum_character("lobby", "grace"), nullptr);
    EXPECT_EQ(workspace.find_forum_character("lobby", "Grace"), nullptr);
    EXPECT_EQ(workspace.find_forum_character("missing", "grace"), nullptr);
}

TEST(Workspace, ResolvesForumHandlesByWordsAndIds) {
    const Workspace workspace = workspace_with_characters({
        {"markus_aurelius", "Marcus Aurelius"},
        {"roosevelt", "Franklin Roosevelt"},
        {"stirlitz", "Штирлиц"},
    });

    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "Marcus").character->id,
        "markus_aurelius");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "Roose").character->id,
        "roosevelt");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "STIRLITZ.").character->id,
        "stirlitz");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "markus").match,
        HandleMatch::unknown);
}

TEST(Workspace, ReportsAmbiguousForumHandles) {
    const Workspace workspace = workspace_with_characters({
        {"churchill", "Winston Churchill"},
        {"smith", "Winston Smith"},
        {"watson", "William Watson"},
    });

    const HandleResolution word =
        workspace.resolve_forum_handle("lobby", "Winston");
    EXPECT_EQ(word.match, HandleMatch::ambiguous);
    EXPECT_EQ(word.candidates.size(), 2U);

    const HandleResolution prefix =
        workspace.resolve_forum_handle("lobby", "W");
    EXPECT_EQ(prefix.match, HandleMatch::ambiguous);
    EXPECT_EQ(prefix.candidates.size(), 3U);
    EXPECT_EQ(
        workspace.resolve_forum_handle("missing", "Winston").match,
        HandleMatch::unknown);
}

TEST(Workspace, PrefersExactForumHandlesAndSupportsUtf8Prefixes) {
    const Workspace workspace = workspace_with_characters({
        {"dotted", "Ismael."},
        {"plain", "Ismael"},
        {"stirlitz", "Штирлиц"},
        {"winston", "Franklin Roosevelt"},
        {"churchill", "Winston Churchill"},
    });

    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "Ismael.").character->id,
        "dotted");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "Ismael,").character->id,
        "plain");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "Winston").character->id,
        "winston");
    EXPECT_EQ(
        workspace.resolve_forum_handle("lobby", "Штир").character->id,
        "stirlitz");
}

std::string subscription_provider_toml(std::string_view extra = {}) {
    std::string text =
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\n"
        "port = 443\n"
        "https = true\n"
        "base_path = \"/backend-api/codex\"\n"
        "mode = \"net\"\n"
        "api = \"responses\"\n"
        "model = \"gpt-5.6-terra\"\n"
        "stream = true\n"
        "web_search = \"off\"\n"
        "cache_retention = \"off\"\n";
    text += extra;
    return text;
}

TEST(Workspace, LoadsOpenAiSubscriptionProvider) {
    test::TestWorkspace fixture;
    fixture.write_provider("chatgpt", subscription_provider_toml());
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"chatgpt\"\n");

    const Workspace workspace = Workspace::load(fixture.root());
    const WorkspaceProvider* const provider = workspace.find_provider("chatgpt");
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->config.auth, ProviderAuth::openai_subscription);
    EXPECT_EQ(provider->config.host, "chatgpt.com");
    EXPECT_EQ(provider->config.port, 443);
    EXPECT_TRUE(provider->config.https);
    EXPECT_EQ(provider->config.base_path, "/backend-api/codex");
    EXPECT_EQ(provider->config.mode, Mode::net);
    EXPECT_EQ(provider->config.api, ProviderApi::responses);
    EXPECT_EQ(provider->config.model, "gpt-5.6-terra");
    EXPECT_TRUE(provider->config.stream);
    EXPECT_TRUE(provider->config.api_key_id.empty());
    EXPECT_FALSE(provider->config.temperature);
    EXPECT_FALSE(provider->config.max_tokens);
    EXPECT_EQ(provider->config.web_search, WebSearchMode::off);
    EXPECT_EQ(provider->config.cache_retention, CacheRetention::off);
    EXPECT_EQ(
        workspace.character_definition("lobby", "guide").provider.config.auth,
        ProviderAuth::openai_subscription);
}

TEST(Workspace, RejectsUnknownProviderAuth) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "chatgpt",
        "auth = \"oauth\"\n"
        "host = \"chatgpt.com\"\n"
        "port = 443\n"
        "mode = \"net\"\n"
        "model = \"gpt-5.6-terra\"\n");
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"chatgpt\"\n");
    EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
}

TEST(Workspace, RejectsInvalidOpenAiSubscriptionSettings) {
    const std::vector<std::string> invalid{
        subscription_provider_toml("temperature = 0.2\n"),
        subscription_provider_toml("max_tokens = 128\n"),
        subscription_provider_toml("api_key = \"api_key_1\"\n"),
        "auth = \"openai_subscription\"\n"
        "host = \"api.openai.com\"\nport = 443\nhttps = true\n"
        "base_path = \"/backend-api/codex\"\nmode = \"net\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"off\"\ncache_retention = \"off\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 80\nhttps = true\n"
        "base_path = \"/backend-api/codex\"\nmode = \"net\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"off\"\ncache_retention = \"off\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 443\nhttps = false\n"
        "base_path = \"/backend-api/codex\"\nmode = \"net\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"off\"\ncache_retention = \"off\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 443\nhttps = true\n"
        "base_path = \"/v1\"\nmode = \"net\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"off\"\ncache_retention = \"off\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 443\nhttps = true\n"
        "base_path = \"/backend-api/codex\"\nmode = \"test\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"off\"\ncache_retention = \"off\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 443\nhttps = true\n"
        "base_path = \"/backend-api/codex\"\nmode = \"net\"\n"
        "api = \"chat_completions\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"off\"\ncache_retention = \"off\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 443\nhttps = true\n"
        "base_path = \"/backend-api/codex\"\nmode = \"net\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = false\n"
        "web_search = \"off\"\ncache_retention = \"off\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 443\nhttps = true\n"
        "base_path = \"/backend-api/codex\"\nmode = \"net\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"auto\"\ncache_retention = \"off\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 443\nhttps = true\n"
        "base_path = \"/backend-api/codex\"\nmode = \"net\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"off\"\ncache_retention = \"short\"\n",
        "auth = \"openai_subscription\"\n"
        "host = \"chatgpt.com\"\nport = 443\nhttps = true\n"
        "base_path = \"/backend-api/codex\"\nmode = \"net\"\n"
        "api = \"responses\"\nmodel = \"gpt-5.6-terra\"\nstream = true\n"
        "web_search = \"off\"\n",
    };
    for (const std::string& contents : invalid) {
        SCOPED_TRACE(contents);
        test::TestWorkspace fixture;
        fixture.write_provider("chatgpt", contents);
        fixture.write_character_config(
            "display_name = \"Guide\"\nprovider = \"chatgpt\"\n");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
}

TEST(Workspace, RejectsOpenAiSubscriptionWebSearchOverrides) {
    {
        test::TestWorkspace fixture;
        fixture.write_provider("chatgpt", subscription_provider_toml());
        fixture.write_character_config(
            "display_name = \"Guide\"\n"
            "provider = \"chatgpt\"\n"
            "web_search = \"auto\"\n");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        fixture.write_provider("chatgpt", subscription_provider_toml());
        fixture.write_character_config(
            "display_name = \"Guide\"\nprovider = \"test\"\n");
        std::ofstream(fixture.root() / "system" / "assistant" / "character.toml")
            << "display_name = \"Assistant\"\n"
               "provider = \"chatgpt\"\n"
               "web_search = \"required\"\n";
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        fixture.write_provider("chatgpt", subscription_provider_toml());
        fixture.write_character_config(
            "display_name = \"Guide\"\nprovider = \"chatgpt\"\n");
        const Workspace workspace = Workspace::load(fixture.root());
        EXPECT_THROW(
            edit_fixture(workspace, [&](WorkspaceConfigEditor& editor) {
                editor.write_character_settings(
                    "guide", "chatgpt", std::nullopt, std::nullopt, std::nullopt,
                    WebSearchMode::automatic);
            }),
            std::invalid_argument);
    }
}

} // namespace
} // namespace cha
