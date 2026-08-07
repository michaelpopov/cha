#include "application/workspace_model.h"

#include "application/builtins.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace cha {
namespace {

WorkspaceModel load_model(const std::filesystem::path& root) {
    return WorkspaceModel::load(root, load_workspace_config(root));
}

std::vector<std::string> display_names(std::span<const CharacterDefinitionMetadata> values) {
    std::vector<std::string> names;
    for (const CharacterDefinitionMetadata& value : values) names.push_back(value.display_name);
    return names;
}

std::vector<std::string> display_names(std::span<const ForumInfo> values) {
    std::vector<std::string> names;
    for (const ForumInfo& value : values) names.push_back(value.display_name);
    return names;
}

// The workspace personas the model published, without the built-in Guest it
// always puts first.
PersonaRoster custom_personas(const WorkspaceModel& model) {
    PersonaRoster roster = *model.personas();
    EXPECT_FALSE(roster.empty());
    EXPECT_EQ(roster.front().id, guest_id);
    roster.erase(roster.begin());
    return roster;
}

// --- Published catalogs ----------------------------------------------------

TEST(WorkspaceModel, LoadsCustomWorkspaceDataAlongsideTheBuiltins) {
    test::TestWorkspace fixture;
    const WorkspaceModel model = load_model(fixture.root());

    ASSERT_NE(model.find_character("guide"), nullptr);
    ASSERT_NE(model.find_character(assistant_id), nullptr);
    ASSERT_NE(model.find_forum("lobby"), nullptr);
    ASSERT_NE(model.find_forum(entrance_id), nullptr);
    EXPECT_EQ(model.find_character("absent"), nullptr);
    EXPECT_EQ(model.find_forum("absent"), nullptr);
}

TEST(WorkspaceModel, PublishesGuestFirstAndEveryCatalogInDisplayOrder) {
    test::TestWorkspace fixture;
    fixture.add_persona("author_key", "An Author");
    const WorkspaceModel model = load_model(fixture.root());

    ASSERT_EQ(model.personas()->size(), 3U);
    EXPECT_EQ(model.personas()->front().id, guest_id);
    EXPECT_EQ((*model.personas())[1].display_name, "An Author");
    EXPECT_EQ((*model.personas())[2].display_name, "Reader");
    EXPECT_EQ(display_names(model.characters()), (std::vector<std::string>{"Assistant", "Guide"}));
    EXPECT_EQ(display_names(model.forums()), (std::vector<std::string>{"Entrance", "The Lobby"}));
}

TEST(WorkspaceModel, ReportsForumMembershipAndDefaultsWithoutAPath) {
    test::TestWorkspace fixture;
    const WorkspaceModel model = load_model(fixture.root());

    const ForumInfo* lobby = model.find_forum("lobby");
    ASSERT_NE(lobby, nullptr);
    EXPECT_EQ(lobby->display_name, "The Lobby");
    EXPECT_EQ(lobby->member_ids, (std::vector<std::string>{"guide"}));
    EXPECT_EQ(lobby->default_character_id, "guide");

    const ForumInfo* entrance = model.find_forum(entrance_id);
    ASSERT_NE(entrance, nullptr);
    EXPECT_EQ(entrance->member_ids, (std::vector<std::string>{std::string(assistant_id)}));
    EXPECT_EQ(entrance->default_character_id, assistant_id);
}

TEST(WorkspaceModel, ServesAssistantDetailFromTheEmbeddedApplicationGuide) {
    test::TestWorkspace fixture;
    const WorkspaceModel model = load_model(fixture.root());

    EXPECT_EQ(model.character_markdown(assistant_id), application_guide());
    EXPECT_EQ(model.character_markdown("guide"), "Character instructions\n");
    EXPECT_THROW((void)model.character_markdown("absent"), std::runtime_error);
}

TEST(WorkspaceModel, BuildsAssistantFromAWorkspaceCarryingDescriptionsAndTags) {
    test::TestWorkspace fixture;
    fixture.write_character_config(
        "display_name = \"Guide\"\ndescription = \"Shows the way\"\ntags = [\"help\"]\n");
    std::ofstream(fixture.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndescription = \"Where visitors arrive\"\n";

    EXPECT_NO_THROW((void)load_model(fixture.root()));
}

TEST(WorkspaceModel, ReturnsOneSessionDirectoryPerCustomForum) {
    test::TestWorkspace fixture;
    const WorkspaceModel model = load_model(fixture.root());

    const std::vector<ForumSessionDirectory> directories = model.session_directories();
    ASSERT_EQ(directories.size(), 1U);
    EXPECT_EQ(directories.front().forum_id, "lobby");
    EXPECT_EQ(
        directories.front().directory,
        fixture.root() / "forums" / "lobby" / "sessions");
}

TEST(WorkspaceModel, FailsToLoadWhenAnyConfiguredForumHasABrokenPrompt) {
    test::TestWorkspace fixture;
    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "$$(missing.md)";

    try {
        (void)load_model(fixture.root());
        FAIL() << "expected a missing prompt include to fail model loading";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("lobby"), std::string::npos) << message;
        EXPECT_NE(message.find("guide"), std::string::npos) << message;
        EXPECT_NE(message.find("cannot read included file"), std::string::npos) << message;
    }
}

TEST(WorkspaceModel, RejectsCharacterAndForumIdsReservedForBuiltins) {
    {
        test::TestWorkspace fixture;
        const auto definition = fixture.root() / "characters" / std::string(guest_id);
        std::filesystem::create_directories(definition);
        std::filesystem::create_directories(
            fixture.root() / "forums" / "lobby" / "members" / std::string(guest_id));
        std::ofstream(definition / "character.toml") << "display_name = \"Impostor\"\n";
        std::ofstream(definition / "CHARACTER.md") << "Prompt\n";
        EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        const auto forum = fixture.root() / "forums" / std::string(entrance_id);
        std::filesystem::create_directories(forum / "members" / "guide");
        std::ofstream(forum / "FORUM.md") << "Forum";
        std::ofstream(forum / "config.toml") << "display_name = \"Other\"\n";
        EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
    }
}

TEST(WorkspaceModel, RejectsAWorkspaceWithoutAPersonasDirectory) {
    test::TestWorkspace fixture;
    std::filesystem::remove_all(fixture.root() / "personas");
    EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
}

TEST(WorkspaceModel, RejectsMalformedForumsButAcceptsAWorkspaceWithNoCustomForum) {
    {
        test::TestWorkspace fixture;
        std::filesystem::create_directories(fixture.root() / "forums" / "broken" / "members");
        std::ofstream(fixture.root() / "forums" / "broken" / "config.toml")
            << "display_name = \"Broken\"\n";
        std::filesystem::create_directories(
            fixture.root() / "forums" / "bad-config" / "members" / "guide");
        std::ofstream(fixture.root() / "forums" / "bad-config" / "config.toml")
            << "display_name = [not valid TOML";
        EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
    }
    {
        test::TestWorkspace fixture;
        std::filesystem::remove_all(fixture.root() / "forums" / "lobby");
        const WorkspaceModel model = load_model(fixture.root());
        EXPECT_EQ(display_names(model.forums()), (std::vector<std::string>{"Entrance"}));
        EXPECT_TRUE(model.session_directories().empty());
    }
}

TEST(WorkspaceModel, RejectsDuplicateAndReservedForumPublicNames) {
    test::TestWorkspace fixture;
    const auto forum = fixture.root() / "forums" / "other";
    std::filesystem::create_directories(forum / "members" / "guide");
    std::ofstream(forum / "FORUM.md") << "Forum";
    std::ofstream(forum / "config.toml") << "display_name = \"the lobby\"\n";
    EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
    std::ofstream(forum / "config.toml") << "display_name = \"Entrance\"\n";
    EXPECT_THROW((void)load_model(fixture.root()), std::runtime_error);
}

TEST(WorkspaceModel, KeepsPublicResultsFixedWhenWorkspaceFilesChangeAfterLoading) {
    test::TestWorkspace fixture;
    const WorkspaceModel model = load_model(fixture.root());
    const std::vector<std::string> characters = display_names(model.characters());
    const std::vector<std::string> forums = display_names(model.forums());
    const std::size_t persona_count = model.personas()->size();

    fixture.write_character_config("display_name = \"Renamed\"\n");
    std::ofstream(fixture.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"Renamed Lobby\"\n";
    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "Edited instructions\n";
    fixture.add_persona("writer", "Writer");

    EXPECT_EQ(display_names(model.characters()), characters);
    EXPECT_EQ(display_names(model.forums()), forums);
    EXPECT_EQ(model.personas()->size(), persona_count);
    EXPECT_EQ(model.find_character("guide")->display_name, "Guide");
    EXPECT_EQ(model.find_forum("lobby")->display_name, "The Lobby");
    EXPECT_EQ(model.character_markdown("guide"), "Character instructions\n");

    const WorkspaceModel reloaded = load_model(fixture.root());
    EXPECT_EQ(reloaded.find_character("guide")->display_name, "Renamed");
    EXPECT_EQ(reloaded.find_forum("lobby")->display_name, "Renamed Lobby");
    EXPECT_EQ(reloaded.character_markdown("guide"), "Edited instructions\n");
    EXPECT_EQ(reloaded.personas()->size(), persona_count + 1);
}

// --- Workspace layout and validation ---------------------------------------

// A hand-built workspace, so each test can break exactly one part of the
// directory graph the model validates at load.
class WorkspaceModelLayoutTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_workspace_model_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "characters" / "guide");
        std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "guide");
        std::filesystem::create_directories(root_ / "personas" / "operator");
        std::ofstream(root_ / "workspace.toml")
            << "[provider]\n"
               "host = \"127.0.0.1\"\nport = 8080\nmode = \"test\"\n"
               "[logging]\n"
               "file = \"logs/cha.log\"\n"
               "level = \"off\"\n";
        std::ofstream(root_ / "forums" / "lobby" / "config.toml")
            << "display_name = \"The Lobby\"\n";
        std::ofstream(root_ / "forums" / "lobby" / "FORUM.md") << "Forum instructions";
        std::ofstream(root_ / "forums" / "lobby" / "members" / "character_defaults.toml")
            << "host = \"127.0.0.1\"\nport = 8080\n";
        std::ofstream(root_ / "characters" / "guide" / "character.toml")
            << "display_name = \"Guide\"\n";
        std::ofstream(root_ / "characters" / "guide" / "CHARACTER.md")
            << "Character instructions";
        std::ofstream(root_ / "personas" / "operator" / "persona.toml")
            << "display_name = \"Reader\"\n";
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    WorkspaceModel load() const { return load_model(root_); }

    std::filesystem::path root_;
};

TEST_F(WorkspaceModelLayoutTest, ResolvesLoggingSettingsRelativeToTheWorkspace) {
    const WorkspaceConfig config = load_workspace_config(root_);
    EXPECT_EQ(config.log_file, root_ / "logs" / "cha.log");
    EXPECT_EQ(config.log_level, "off");
    EXPECT_NO_THROW((void)WorkspaceModel::load(root_, config));
}

TEST_F(WorkspaceModelLayoutTest, LeavesAnAbsoluteLogPathUntouched) {
    const std::filesystem::path absolute_log = root_ / "elsewhere" / "cha.log";
    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"test\"\nport = 1\nmode = \"test\"\n"
           "[logging]\nfile = \"" << absolute_log.string()
        << "\"\nlevel = \"off\"\n";

    EXPECT_EQ(load_workspace_config(root_).log_file, absolute_log);
}

TEST_F(WorkspaceModelLayoutTest, KeepsBindAndProviderConfigurationDistinct) {
    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"provider.example\"\nport = 444\nmode = \"test\"\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    const WorkspaceConfig config = load_workspace_config(root_);
    EXPECT_EQ(*config.provider.host, "provider.example");
    EXPECT_EQ(*config.provider.port, 444);
    EXPECT_NO_THROW((void)WorkspaceModel::load(root_, config));
}

TEST_F(WorkspaceModelLayoutTest, RequiresValidProviderWithoutAcceptingIdentityFields) {
    std::ofstream(root_ / "workspace.toml")
        << "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);

    std::ofstream(root_ / "workspace.toml")
        << "[provider]\ndisplay_name = \"No\"\nhost = \"test\"\nport = 1\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);

    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"test\"\nport = 1\nhtps = true\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);

    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"test\"\nport = 1\napi_key = \"secret\"\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceModelLayoutTest, RequiresWorkspaceConfiguration) {
    std::filesystem::remove(root_ / "workspace.toml");

    try {
        (void)load();
        FAIL() << "expected missing workspace config to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("workspace.toml"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, RequiresValidLoggingConfiguration) {
    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"test\"\nport = 1\nmode = \"test\"\n";
    EXPECT_THROW((void)load_workspace_config(root_), std::runtime_error);

    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"test\"\nport = 1\nmode = \"test\"\n"
           "[logging]\nfile = \"\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)load_workspace_config(root_), std::runtime_error);
}

TEST_F(WorkspaceModelLayoutTest, RequiresWorkspaceDefinitions) {
    std::filesystem::remove_all(root_ / "characters");
    try {
        (void)load();
        FAIL() << "expected missing definitions directory to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(root_.string()), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("characters/"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, RequiresAForumsDirectory) {
    std::filesystem::remove_all(root_ / "forums");
    try {
        (void)load();
        FAIL() << "expected missing forums directory to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("forums/"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, RequiresMembersDirectory) {
    std::filesystem::remove_all(root_ / "forums" / "lobby" / "members");
    try {
        (void)load();
        FAIL() << "expected missing members directory to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("members/"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, RejectsDanglingMember) {
    std::filesystem::rename(
        root_ / "forums" / "lobby" / "members" / "guide",
        root_ / "forums" / "lobby" / "members" / "unknown");
    try {
        (void)load();
        FAIL() << "expected dangling member to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("unknown"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, RejectsInvalidMemberIdBeforeReferenceValidation) {
    std::filesystem::rename(
        root_ / "forums" / "lobby" / "members" / "guide",
        root_ / "forums" / "lobby" / "members" / "guide.invalid");
    try {
        (void)load();
        FAIL() << "expected invalid member ID to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("guide.invalid"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Character ID"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, ValidatesRequiredDefinitionFiles) {
    std::filesystem::remove(root_ / "characters" / "guide" / "character.toml");
    EXPECT_THROW((void)load(), std::runtime_error);

    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"Guide\"\n";
    std::filesystem::remove(root_ / "characters" / "guide" / "CHARACTER.md");
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceModelLayoutTest, RejectsMalformedDefinition) {
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = [\"Guide\"]\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceModelLayoutTest, RejectsInvalidDefinitionId) {
    std::filesystem::rename(
        root_ / "characters" / "guide", root_ / "characters" / "guide.invalid");
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceModelLayoutTest, RejectsReservedDefinitionDisplayName) {
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"system\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceModelLayoutTest, RejectsDuplicateCharacterDisplayNames) {
    const std::filesystem::path duplicate = root_ / "characters" / "other";
    std::filesystem::create_directories(duplicate);
    std::ofstream(duplicate / "character.toml") << "display_name = \"gUiDe\"\n";
    std::ofstream(duplicate / "CHARACTER.md") << "Other instructions";
    try {
        (void)load();
        FAIL() << "expected duplicate character display name rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("gUiDe"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("public name"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, AcceptsEmptyMemberDirectoriesAndOrphanDefinitions) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "members" / "guide" / "character.toml");
    std::filesystem::create_directories(root_ / "characters" / "orphan");
    std::ofstream(root_ / "characters" / "orphan" / "character.toml")
        << "display_name = \"Orphan\"\n";
    std::ofstream(root_ / "characters" / "orphan" / "CHARACTER.md") << "Orphan";
    EXPECT_NO_THROW((void)load());
}

TEST_F(WorkspaceModelLayoutTest, AllowsOneDefinitionInMultipleForums) {
    const std::filesystem::path other = root_ / "forums" / "other";
    std::filesystem::create_directories(other / "members" / "guide");
    std::ofstream(other / "config.toml") << "display_name = \"Other\"\n";
    std::ofstream(other / "FORUM.md") << "Other forum";
    EXPECT_NO_THROW((void)load());
}

TEST_F(WorkspaceModelLayoutTest, EnumeratesForumsInNameOrderAndIgnoresUnsafeDirectories) {
    for (const std::string_view name : {"alpha", "zulu", "bad space", "bad#fragment"}) {
        const std::filesystem::path forum = root_ / "forums" / std::string(name);
        std::filesystem::create_directories(forum / "members" / "guide");
        std::ofstream(forum / "config.toml")
            << "display_name = \"Forum " << name << "\"\n";
        std::ofstream(forum / "FORUM.md") << "Forum";
    }
    const WorkspaceModel model = load();

    EXPECT_EQ(
        display_names(model.forums()),
        (std::vector<std::string>{"Entrance", "Forum alpha", "Forum zulu", "The Lobby"}));
    EXPECT_EQ(model.session_directories().size(), 3U);
    for (const std::string_view id : {"alpha", "lobby", "zulu"}) {
        EXPECT_NE(model.find_forum(id), nullptr) << id;
    }
    EXPECT_EQ(model.find_forum("bad space"), nullptr);
    EXPECT_EQ(model.find_forum("bad#fragment"), nullptr);
}

TEST_F(WorkspaceModelLayoutTest, ResolvesDefaultAgentWithoutReorderingMembers) {
    std::filesystem::create_directories(root_ / "characters" / "alpha");
    std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "alpha");
    std::ofstream(root_ / "characters" / "alpha" / "character.toml")
        << "display_name = \"Alpha\"\n";
    std::ofstream(root_ / "characters" / "alpha" / "CHARACTER.md") << "Alpha";
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_agent = \"guide\"\n";

    const WorkspaceModel model = load();
    const ForumInfo* const forum = model.find_forum("lobby");
    ASSERT_NE(forum, nullptr);
    EXPECT_EQ(forum->member_ids, (std::vector<std::string>{"alpha", "guide"}));
    EXPECT_EQ(forum->default_character_id, "guide");
}

TEST_F(WorkspaceModelLayoutTest, DefaultsToFirstLexicographicMember) {
    std::filesystem::create_directories(root_ / "characters" / "alpha");
    std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "alpha");
    std::ofstream(root_ / "characters" / "alpha" / "character.toml")
        << "display_name = \"Alpha\"\n";
    std::ofstream(root_ / "characters" / "alpha" / "CHARACTER.md") << "Alpha";

    const WorkspaceModel model = load();
    const ForumInfo* const forum = model.find_forum("lobby");
    ASSERT_NE(forum, nullptr);
    EXPECT_EQ(forum->default_character_id, "alpha");
}

TEST_F(WorkspaceModelLayoutTest, RejectsUnknownAndMalformedDefaultAgent) {
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_agent = \"missing\"\n";
    EXPECT_THROW((void)load(), std::runtime_error);

    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_agent = [\"guide\"]\n";
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceModelLayoutTest, RequiresPersonaDirectoryButAllowsItEmpty) {
    std::filesystem::remove_all(root_ / "personas");
    EXPECT_THROW((void)load(), std::runtime_error);

    std::filesystem::create_directories(root_ / "personas");
    EXPECT_TRUE(custom_personas(load()).empty());
}

TEST_F(WorkspaceModelLayoutTest, PersonaLoadingDoesNotSkipMalformedDirectories) {
    std::filesystem::create_directories(root_ / "personas" / "missing_config");
    EXPECT_THROW((void)load(), std::runtime_error);
}

TEST_F(WorkspaceModelLayoutTest, RejectsPersonaCharacterIdCollision) {
    std::filesystem::rename(root_ / "personas" / "operator", root_ / "personas" / "guide");
    try {
        (void)load();
        FAIL() << "expected persona/character ID collision rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("guide"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Persona"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("character"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, RejectsPersonaCharacterDisplayNameCollision) {
    std::ofstream(root_ / "personas" / "operator" / "persona.toml")
        << "display_name = \"gUiDe\"\n";
    try {
        (void)load();
        FAIL() << "expected persona/character display-name collision rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("gUiDe"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Guide"), std::string::npos);
    }
}

TEST_F(WorkspaceModelLayoutTest, AcceptsDefinitionDefaultsWithAMemberOverride) {
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"Guide\"\nhost = \"127.0.0.1\"\n";
    std::ofstream(root_ / "forums" / "lobby" / "members" / "character_defaults.toml")
        << "port = 8080\n";
    std::ofstream(root_ / "forums" / "lobby" / "members" / "guide" / "character.toml")
        << "[prompt]\nvoice = \"member\"\n";
    std::ofstream(root_ / "characters" / "guide" / "CHARACTER.md") << "$${voice}";

    EXPECT_NO_THROW((void)load());
}

TEST_F(WorkspaceModelLayoutTest, InheritsApplicationProviderSettingsWithoutSharedDefaults) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "members" / "character_defaults.toml");
    EXPECT_NO_THROW((void)load());
}

// --- Persona loading --------------------------------------------------------

class WorkspaceModelPersonaTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_model_personas_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "forums");
        std::filesystem::create_directories(root_ / "characters" / "guide");
        std::ofstream(root_ / "characters" / "guide" / "character.toml")
            << "display_name = \"Guide\"\n";
        std::ofstream(root_ / "characters" / "guide" / "CHARACTER.md") << "Guide";
        std::ofstream(root_ / "workspace.toml")
            << "[provider]\n"
               "host = \"test\"\nport = 1\nmode = \"test\"\n"
               "[logging]\n"
               "file = \"cha.log\"\n"
               "level = \"off\"\n";
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    void add_persona(
        std::string_view id,
        std::string_view display_name,
        std::string_view prompt = "") const {
        const std::filesystem::path directory = root_ / "personas" / std::string(id);
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "persona.toml")
            << "display_name = \"" << display_name << "\"\n";
        if (!prompt.empty()) std::ofstream(directory / "PERSONA.md") << prompt;
    }

    PersonaRoster personas() const { return custom_personas(load_model(root_)); }

    std::filesystem::path root_;
};

TEST_F(WorkspaceModelPersonaTest, PublishesPersonasInDisplayNameOrderAndReadsOptionalPrompt) {
    add_persona("zebra", "Zebra", "Verbatim\ntext");
    add_persona("alpha", "Alpha");

    const PersonaRoster roster = personas();

    ASSERT_EQ(roster.size(), 2U);
    EXPECT_EQ(roster[0].id, "alpha");
    EXPECT_EQ(roster[0].display_name, "Alpha");
    EXPECT_TRUE(roster[0].prompt.empty());
    EXPECT_EQ(roster[1].id, "zebra");
    EXPECT_EQ(roster[1].display_name, "Zebra");
    EXPECT_EQ(roster[1].prompt, "Verbatim\ntext");
}

TEST_F(WorkspaceModelPersonaTest, LoadsOptionalDescriptionAndRejectsMalformedDescription) {
    add_persona("ada", "Ada");
    const auto config = root_ / "personas" / "ada" / "persona.toml";
    std::ofstream(config) << "display_name = \"Ada\"\ndescription = \"A careful reader\"\n";
    const PersonaRoster roster = personas();
    ASSERT_TRUE(roster[0].description);
    EXPECT_EQ(*roster[0].description, "A careful reader");

    std::ofstream(config) << "display_name = \"Ada\"\ndescription = \" bad\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceModelPersonaTest, RequiresAPersonaTomlForEveryPersonaDirectory) {
    std::filesystem::create_directories(root_ / "personas" / "missing");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceModelPersonaTest, RequiresPersonaPromptToBeARegularFile) {
    add_persona("ada", "Ada");
    std::filesystem::create_directory(root_ / "personas" / "ada" / "PERSONA.md");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceModelPersonaTest, RejectsUnknownOrMissingOrWrongTypedConfigFields) {
    add_persona("ada", "Ada");
    const auto config = root_ / "personas" / "ada" / "persona.toml";

    std::ofstream(config) << "display_name = \"Ada\"\nextra = true\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "other = \"Ada\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = 42\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceModelPersonaTest, RejectsInvalidAndReservedIds) {
    add_persona("bad-id", "Ada");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::filesystem::remove_all(root_ / "personas");
    add_persona("9ada", "Ada");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::filesystem::remove_all(root_ / "personas");
    add_persona("System", "Ada");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceModelPersonaTest, RejectsInvalidReservedAndDuplicateDisplayNames) {
    add_persona("ada", " Ada");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    const auto config = root_ / "personas" / "ada" / "persona.toml";
    std::ofstream(config) << "display_name = \"@Ada\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"System\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ada\\u0001\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ada\"\n";
    add_persona("other", "aDA");
    EXPECT_THROW((void)load_model(root_), std::runtime_error);
}

TEST_F(WorkspaceModelPersonaTest, RejectsUnicodeControlsLineBreaksAndBoundaryWhitespace) {
    add_persona("ada", "Ada");
    const std::filesystem::path config = root_ / "personas" / "ada" / "persona.toml";

    std::ofstream(config) << "display_name = \"Ada\\u0085Lovelace\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ada\\u2028Lovelace\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"\\u00a0Ada\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ada\\u3000\"\n";
    EXPECT_THROW((void)load_model(root_), std::runtime_error);

    std::ofstream(config) << "display_name = \"Ren\\u00e9e \\u674e\"\n";
    const PersonaRoster roster = personas();
    ASSERT_EQ(roster.size(), 1U);
    EXPECT_EQ(roster.front().display_name, "Renée 李");
}

} // namespace
} // namespace cha
