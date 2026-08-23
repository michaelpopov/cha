#include "workspace/workspace.h"

#include "support/test_workspace.h"
#include "util/environment.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>

namespace cha {
namespace {

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
    fixture.write_character_config(
        "display_name = \"Guide\"\n"
        "description = \"A helpful guide.\"\n"
        "provider = \"test\"\n"
        "style = \"serif\"\n"
        "tags = [\"help\"]\n"
        "[prompt]\n"
        "greeting = \"Hello\"\n");
    std::ofstream(
        fixture.root() / "forums" / "lobby" / "members"
            / "character_defaults.toml")
        << "[prompt]\ngreeting = \"Welcome\"\n";
    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "$${greeting}, I am $${character.display_name} in "
           "$${forum.display_name}.\n"
           "<character_profile>\nIntrinsic Guide.\n</character_profile>\n";

    const Workspace workspace = Workspace::load(fixture.root());

    EXPECT_EQ(workspace.settings().log_level, "off");
    ASSERT_NE(workspace.find_provider("test"), nullptr);
    EXPECT_EQ(workspace.find_provider("test")->config.model, "fake");
    EXPECT_EQ(workspace.find_provider("test")->label, "Test");
    ASSERT_NE(workspace.find_style("serif"), nullptr);
    EXPECT_EQ(workspace.find_style("serif")->label, "Serif");
    EXPECT_EQ(
        workspace.find_style("serif")->appearance.font,
        CharacterFont::serif);
    ASSERT_NE(workspace.find_persona("reader"), nullptr);
    ASSERT_NE(workspace.find_character("guide"), nullptr);
    EXPECT_EQ(workspace.find_character("guide")->provider_id, "test");
    EXPECT_EQ(
        workspace.find_character("guide")->character.appearance.weight,
        CharacterWeight::bold);
    ASSERT_NE(workspace.find_forum("lobby"), nullptr);
    ASSERT_EQ(workspace.find_forum("lobby")->members.size(), 1U);
    ASSERT_NE(workspace.find_forum_member("lobby", "guide"), nullptr);
    EXPECT_EQ(
        workspace.forum_session_directory("lobby"),
        fixture.root() / "forums" / "lobby" / "sessions");
    EXPECT_FALSE(
        workspace.forum_session_directory(workspace_entrance_id));
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

TEST(Workspace, OmitsAnInvalidUnusedProvider) {
    test::TestWorkspace fixture;
    fixture.write_provider("unused", "host = \"localhost\"\nport = 80\n");

    const Workspace workspace = Workspace::load(fixture.root());
    EXPECT_EQ(workspace.find_provider("unused"), nullptr);
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
        "style = \"loud-style\"\n");

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
    EXPECT_FALSE(provider->config.stream);
    EXPECT_EQ(provider->config.temperature, 0.25);
    EXPECT_EQ(provider->config.max_tokens, 512);
    EXPECT_EQ(provider->config.reasoning_format, ReasoningFormat::reasoning);
    EXPECT_EQ(provider->config.web_search, WebSearchMode::automatic);
    EXPECT_EQ(provider->config.cache_retention, CacheRetention::long_);

    const WorkspaceStyle* const style = workspace.find_style("loud-style");
    ASSERT_NE(style, nullptr);
    EXPECT_EQ(style->label, "Loud style");
    EXPECT_EQ(style->appearance.font, CharacterFont::mono);
    EXPECT_EQ(style->appearance.style, CharacterSlant::italic);
    EXPECT_EQ(style->appearance.weight, CharacterWeight::bold);
    EXPECT_EQ(style->appearance.size, CharacterScale::large);
    EXPECT_EQ(style->appearance.text_color, CharacterTextColor::accent);

    const ModelBackendConfig& runtime = provider->config;
    EXPECT_EQ(runtime.model, "model-one");
    EXPECT_EQ(runtime.api, ProviderApi::responses);
    EXPECT_EQ(runtime.web_search, WebSearchMode::automatic);
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
            / "CHARACTER.md")
        << "Member prompt: $${voice}\n";

    const Workspace workspace = Workspace::load(fixture.root());
    const WorkspaceForumMember* const member =
        workspace.find_forum_member("lobby", "guide");
    ASSERT_NE(member, nullptr);
    EXPECT_EQ(member->character_prompt, "Member prompt: member\n");
    EXPECT_NE(member->system_prompt.find("READER_ONLY_BODY"), std::string::npos);
    EXPECT_EQ(member->system_prompt.find("AUTHOR_ONLY_BODY"), std::string::npos);
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
        std::filesystem::remove(fixture.root() / "workspace.toml");
        EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
    }
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

TEST(Workspace, ResolvesRelativeAndAbsoluteLoggingPaths) {
    test::TestWorkspace relative;
    const Workspace relative_workspace = Workspace::load(relative.root());
    EXPECT_EQ(
        relative_workspace.settings().log_file,
        relative.root() / "logs" / "cha.log");

    test::TestWorkspace absolute;
    const std::filesystem::path log = absolute.root() / "absolute.log";
    std::ofstream(absolute.root() / "workspace.toml")
        << "[logging]\nfile = \"" << log.string()
        << "\"\nlevel = \"debug\"\n";
    const Workspace absolute_workspace = Workspace::load(absolute.root());
    EXPECT_EQ(absolute_workspace.settings().log_file, log);
    EXPECT_EQ(absolute_workspace.settings().log_level, "debug");
}

TEST(Workspace, RejectsASelectedProviderWithAnUnavailableCredential) {
    test::TestWorkspace fixture;
    constexpr std::string_view variable =
        "CHA_WORKSPACE_TEST_MISSING_CREDENTIAL_71C1";
    ASSERT_TRUE(unset_environment_variable(variable));
    fixture.write_provider(
        "secured",
        "host = \"example.test\"\n"
        "port = 443\n"
        "mode = \"net\"\n"
        "model = \"secured\"\n"
        "api_key_env = \"CHA_WORKSPACE_TEST_MISSING_CREDENTIAL_71C1\"\n");
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"secured\"\n");

    try {
        (void)Workspace::load(fixture.root());
        FAIL() << "Expected the selected provider to be rejected";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(
            message.find("Character 'guide' references invalid provider 'secured'"),
            std::string::npos);
        EXPECT_NE(
            message.find("CHA_WORKSPACE_TEST_MISSING_CREDENTIAL_71C1"),
            std::string::npos);
    }
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
    workspace.write_character_settings(
        "guide", "second", std::string_view{"mono"});
    workspace.write_forum_default_character("lobby", "writer");
    workspace.write_forum_default_persona("lobby", "reader");

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
        workspace.write_character_settings("guide", "missing", std::nullopt),
        std::invalid_argument);
    EXPECT_THROW(
        workspace.write_character_settings(
            "guide", "test", std::string_view{"missing"}),
        std::invalid_argument);
    EXPECT_THROW(
        workspace.write_character_settings(
            workspace_assistant_id, "test", std::nullopt),
        std::runtime_error);
    EXPECT_EQ(file_bytes(character), before);
    EXPECT_TRUE(workspace.character_is_writable("guide"));
    EXPECT_FALSE(workspace.character_is_writable(workspace_assistant_id));
}

TEST(Workspace, LoadwsPublishesOnlyACompleteWorkspace) {
    test::TestWorkspace valid;
    loadws(valid.root());
    const std::shared_ptr<const Workspace> published = getws();
    ASSERT_NE(published, nullptr);
    EXPECT_EQ(published->root(), valid.root());

    test::TestWorkspace invalid;
    invalid.write_character_config("display_name =\n");
    EXPECT_THROW(loadws(invalid.root()), std::runtime_error);
    EXPECT_EQ(getws(), published);
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

} // namespace
} // namespace cha
