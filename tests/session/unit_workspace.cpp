#include "session/session_controller.h"
#include "session/session_lease.h"
#include "session/workspace.h"
#include "support/test_notifier.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <map>
#include <string>
#include <vector>

#ifndef _WIN32
#include "support/lease_test_process.h"
#endif

namespace cha {
namespace {

test::NoopNotifier& notifier() {
    static test::NoopNotifier instance;
    return instance;
}

TEST(SessionIdentity, EqualityAndOrderingUseForumAndSessionIds) {
    const SessionIdentity first{"alpha", "one"};
    const SessionIdentity same{"alpha", "one"};
    const SessionIdentity different_forum{"beta", "one"};
    const SessionIdentity different_session{"alpha", "two"};

    EXPECT_EQ(first, same);
    EXPECT_NE(first, different_forum);
    EXPECT_NE(first, different_session);

    std::map<SessionIdentity, int> ordered{
        {different_forum, 3}, {different_session, 2}, {first, 1}};
    std::vector<SessionIdentity> keys;
    for (const auto& [identity, ignored] : ordered) {
        (void)ignored;
        keys.push_back(identity);
    }
    EXPECT_EQ(keys, (std::vector<SessionIdentity>{first, different_session, different_forum}));
}

class ApplicationWorkspaceTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_workspace_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "characters" / "guide");
        std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "guide");
        std::filesystem::create_directories(root_ / "personas" / "operator");
        {
            std::ofstream workspace_config(root_ / "workspace.toml");
            workspace_config << "[provider]\n"
                       << "host = \"127.0.0.1\"\nport = 8080\nmode = \"test\"\n"
                       << "[logging]\n"
                       << "file = \"logs/cha.log\"\n"
                       << "level = \"off\"\n";
        }
        {
            std::ofstream forum_config(root_ / "forums" / "lobby" / "config.toml");
            forum_config << "display_name = \"The Lobby\"\n";
        }
        {
            std::ofstream forum_prompt(
                root_ / "forums" / "lobby" / "FORUM.md");
            forum_prompt << "Forum instructions";
        }
        {
            std::ofstream base_config(
                root_ / "forums" / "lobby" / "members" / "character_defaults.toml");
            base_config << "host = \"127.0.0.1\"\n"
                        << "port = 8080\n";
        }
        {
            std::ofstream config(
                root_ / "characters" / "guide" / "character.toml");
            config << "display_name = \"Guide\"\n";
        }
        {
            std::ofstream system_prompt(
                root_ / "characters" / "guide" / "CHARACTER.md");
            system_prompt << "Character instructions";
        }
        std::ofstream(root_ / "personas" / "operator" / "persona.toml")
            << "display_name = \"Reader\"\n";
    }

    void TearDown() override {
        std::filesystem::remove_all(root_);
    }

    std::filesystem::path root_;
};

TEST_F(ApplicationWorkspaceTest, ListsForumsAndSessionsAsApplicationValues) {
    Workspace workspace(root_);

    EXPECT_EQ(workspace.forums(), (std::vector<std::string>{"lobby"}));
    EXPECT_TRUE(workspace.sessions("lobby").empty());
    EXPECT_EQ(workspace.workspace_config().log_file, root_ / "logs" / "cha.log");
    EXPECT_EQ(workspace.workspace_config().log_level, "off");
}

TEST_F(ApplicationWorkspaceTest, LeavesAnAbsoluteLogPathUntouched) {
    const std::filesystem::path absolute_log = root_ / "elsewhere" / "cha.log";
    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"test\"\nport = 1\nmode = \"test\"\n"
           "[logging]\nfile = \"" << absolute_log.string()
        << "\"\nlevel = \"off\"\n";

    EXPECT_EQ(load_workspace_config(root_).log_file, absolute_log);
}

TEST_F(ApplicationWorkspaceTest, KeepsBindAndProviderConfigurationDistinct) {
    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"provider.example\"\nport = 444\nmode = \"test\"\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    const Workspace workspace(root_);
    EXPECT_EQ(*workspace.workspace_config().provider.host, "provider.example");
    EXPECT_EQ(*workspace.workspace_config().provider.port, 444);
}

TEST_F(ApplicationWorkspaceTest, RequiresValidProviderWithoutAcceptingIdentityFields) {
    std::ofstream(root_ / "workspace.toml")
        << "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
    std::ofstream(root_ / "workspace.toml")
        << "[provider]\ndisplay_name = \"No\"\nhost = \"test\"\nport = 1\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);

    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"test\"\nport = 1\nhtps = true\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);

    std::ofstream(root_ / "workspace.toml")
        << "[provider]\nhost = \"test\"\nport = 1\napi_key = \"secret\"\n"
           "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n";
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, RequiresWorkspaceDefinitions) {
    std::filesystem::remove_all(root_ / "characters");
    try {
        (void)Workspace(root_);
        FAIL() << "expected missing definitions directory to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(root_.string()), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("characters/"), std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, RequiresMembersDirectory) {
    std::filesystem::remove_all(root_ / "forums" / "lobby" / "members");
    try {
        (void)Workspace(root_);
        FAIL() << "expected missing members directory to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("members/"), std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, RejectsDanglingMember) {
    std::filesystem::rename(
        root_ / "forums" / "lobby" / "members" / "guide",
        root_ / "forums" / "lobby" / "members" / "unknown");
    try {
        (void)Workspace(root_);
        FAIL() << "expected dangling member to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("unknown"), std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, RejectsInvalidMemberIdBeforeReferenceValidation) {
    std::filesystem::rename(
        root_ / "forums" / "lobby" / "members" / "guide",
        root_ / "forums" / "lobby" / "members" / "guide.invalid");
    try {
        (void)Workspace(root_);
        FAIL() << "expected invalid member ID to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("guide.invalid"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Character ID"), std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, ValidatesRequiredDefinitionFilesAtWorkspaceLoad) {
    std::filesystem::remove(root_ / "characters" / "guide" / "character.toml");
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);

    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"Guide\"\n";
    std::filesystem::remove(root_ / "characters" / "guide" / "CHARACTER.md");
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, RejectsMalformedDefinitionAtWorkspaceLoad) {
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = [\"Guide\"]\n";
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, RejectsInvalidDefinitionIdAtWorkspaceLoad) {
    std::filesystem::rename(
        root_ / "characters" / "guide", root_ / "characters" / "guide.invalid");
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, RejectsReservedDefinitionDisplayNameAtWorkspaceLoad) {
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"system\"\n";
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, AcceptsEmptyMemberDirectoriesAndOrphanDefinitions) {
    std::filesystem::remove(root_ / "forums" / "lobby" / "members" / "guide" / "character.toml");
    std::filesystem::create_directories(root_ / "characters" / "orphan");
    std::ofstream(root_ / "characters" / "orphan" / "character.toml")
        << "display_name = \"Orphan\"\n";
    std::ofstream(root_ / "characters" / "orphan" / "CHARACTER.md") << "Orphan";
    EXPECT_NO_THROW((void)Workspace(root_));
}

TEST_F(ApplicationWorkspaceTest, AllowsOneDefinitionInMultipleForums) {
    const std::filesystem::path other = root_ / "forums" / "other";
    std::filesystem::create_directories(other / "members" / "guide");
    std::ofstream(other / "config.toml") << "display_name = \"Other\"\n";
    std::ofstream(other / "FORUM.md") << "Other forum";
    EXPECT_NO_THROW((void)Workspace(root_));
}

TEST_F(ApplicationWorkspaceTest, ResolvesDefaultAgentWithoutReorderingMembers) {
    std::filesystem::create_directories(root_ / "characters" / "alpha");
    std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "alpha");
    std::ofstream(root_ / "characters" / "alpha" / "character.toml")
        << "display_name = \"Alpha\"\n";
    std::ofstream(root_ / "characters" / "alpha" / "CHARACTER.md") << "Alpha";
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_agent = \"guide\"\n";

    Workspace workspace(root_);
    const Forum forum = workspace.load_forum("lobby");
    EXPECT_EQ(forum.character_names, (std::vector<std::string>{"alpha", "guide"}));
    EXPECT_EQ(forum.default_agent_id, "guide");

    OpenedSession created = workspace.create_session("lobby", "default", notifier());
    EXPECT_EQ(created.controller->default_agent_id(), "guide");
    EXPECT_EQ(
        created.descriptor,
        (SessionDescriptor{
            .identity = {"lobby", created.descriptor.identity.session_id},
            .forum_display_name = "The Lobby",
            .session_label = "default",
            .forum_default_character_id = "guide",
        }));
    const std::string session_id = created.descriptor.identity.session_id;
    created.controller->shutdown();
    created.controller.reset();

    OpenedSession opened_session =
        workspace.open_session({"lobby", session_id}, notifier());
    EXPECT_EQ(opened_session.descriptor, created.descriptor);
    std::unique_ptr<SessionController> opened = std::move(opened_session.controller);
    EXPECT_EQ(opened->default_agent_id(), "guide");
    opened->shutdown();
}

TEST_F(ApplicationWorkspaceTest, DefaultsToFirstLexicographicMember) {
    std::filesystem::create_directories(root_ / "characters" / "alpha");
    std::filesystem::create_directories(root_ / "forums" / "lobby" / "members" / "alpha");
    std::ofstream(root_ / "characters" / "alpha" / "character.toml")
        << "display_name = \"Alpha\"\n";
    std::ofstream(root_ / "characters" / "alpha" / "CHARACTER.md") << "Alpha";
    const Forum forum = Workspace(root_).load_forum("lobby");
    EXPECT_EQ(forum.default_agent_id, "alpha");
}

TEST_F(ApplicationWorkspaceTest, RejectsUnknownAndMalformedDefaultAgent) {
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_agent = \"missing\"\n";
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);

    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_agent = [\"guide\"]\n";
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, WorkspaceConstructionRequiresPersonaDirectoryButAllowsItEmpty) {
    std::filesystem::remove_all(root_ / "personas");
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);

    std::filesystem::create_directories(root_ / "personas");
    EXPECT_NO_THROW((void)Workspace(root_));
}

TEST_F(ApplicationWorkspaceTest, PersonaLoadingDoesNotSkipMalformedDirectories) {
    std::filesystem::create_directories(root_ / "personas" / "missing_config");
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, RequiresWorkspaceConfiguration) {
    std::filesystem::remove(root_ / "workspace.toml");

    try {
        (void)Workspace(root_);
        FAIL() << "expected missing workspace config to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("workspace.toml"),
            std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, RequiresValidLoggingConfiguration) {
    {
        std::ofstream workspace_config(root_ / "workspace.toml");
        workspace_config << "[provider]\n"
                         << "host = \"test\"\nport = 1\nmode = \"test\"\n";
    }
    EXPECT_THROW((void)load_workspace_config(root_), std::runtime_error);

    {
        std::ofstream workspace_config(root_ / "workspace.toml");
        workspace_config << "[provider]\n"
                         << "host = \"test\"\nport = 1\nmode = \"test\"\n"
                         << "[logging]\nfile = \"\"\nlevel = \"off\"\n";
    }
    EXPECT_THROW((void)load_workspace_config(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, ChecksAForumWithoutCreatingASession) {
    Workspace workspace(root_);

    const Forum forum = workspace.check_forum("lobby");

    EXPECT_EQ(forum.name, "lobby");
    EXPECT_EQ(forum.display_name, "The Lobby");
    EXPECT_EQ(forum.character_names, (std::vector<std::string>{"guide"}));
    EXPECT_TRUE(workspace.sessions("lobby").empty());
}

TEST_F(ApplicationWorkspaceTest, ForumCheckUsesDefinitionDefaultsAndMemberOverride) {
    std::ofstream(root_ / "characters" / "guide" / "character.toml")
        << "display_name = \"Guide\"\nhost = \"127.0.0.1\"\n";
    std::ofstream(root_ / "forums" / "lobby" / "members" / "character_defaults.toml")
        << "port = 8080\n";
    std::ofstream(root_ / "forums" / "lobby" / "members" / "guide" / "character.toml")
        << "[prompt]\nvoice = \"member\"\n";
    std::ofstream(root_ / "characters" / "guide" / "CHARACTER.md")
        << "$${voice}";

    EXPECT_NO_THROW((void)Workspace(root_).check_forum("lobby"));
}

TEST_F(ApplicationWorkspaceTest, ForumCheckInheritsApplicationProviderSettings) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "members" / "character_defaults.toml");
    Workspace workspace(root_);

    EXPECT_NO_THROW((void)workspace.check_forum("lobby"));
    EXPECT_TRUE(workspace.sessions("lobby").empty());
}

TEST_F(ApplicationWorkspaceTest, ForumCheckExpandsEveryPromptLink) {
    {
        std::ofstream system_prompt(
            root_ / "characters" / "guide" / "CHARACTER.md");
        system_prompt << "$$(missing.md)";
    }
    Workspace workspace(root_);

    try {
        (void)workspace.check_forum("lobby");
        FAIL() << "expected a missing prompt include to fail";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("guide"), std::string::npos) << message;
        EXPECT_NE(message.find("cannot read included file"), std::string::npos)
            << message;
    }
    EXPECT_TRUE(workspace.sessions("lobby").empty());
}

TEST_F(ApplicationWorkspaceTest, WorkspaceConstructionRejectsDuplicateCharacterDisplayNames) {
    const std::filesystem::path duplicate =
        root_ / "characters" / "other";
    std::filesystem::create_directories(duplicate);
    {
        std::ofstream config(duplicate / "character.toml");
        config << "display_name = \"gUiDe\"\n";
        std::ofstream system_prompt(duplicate / "CHARACTER.md");
        system_prompt << "Other instructions";
    }
    try {
        (void)Workspace(root_);
        FAIL() << "expected duplicate character display name rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("gUiDe"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("public name"), std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, WorkspaceConstructionRejectsPersonaCharacterIdCollision) {
    std::filesystem::rename(root_ / "personas" / "operator", root_ / "personas" / "guide");
    try {
        (void)Workspace(root_);
        FAIL() << "expected persona/character ID collision rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("guide"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Persona"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("character"), std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, WorkspaceConstructionRejectsPersonaCharacterDisplayNameCollision) {
    std::ofstream(root_ / "personas" / "operator" / "persona.toml")
        << "display_name = \"gUiDe\"\n";
    try {
        (void)Workspace(root_);
        FAIL() << "expected persona/character display-name collision rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("gUiDe"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("Guide"), std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, CreatesAndReopensAChatSession) {
    Workspace workspace(root_);

    OpenedSession created =
        workspace.create_session(
            "lobby",
            "Browser-ready session",
            notifier());
    const std::string created_id = created.descriptor.identity.session_id;
    created.controller->shutdown();
    created.controller.reset();

    const std::vector<SessionSummary> sessions =
        workspace.sessions("lobby");
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_FALSE(sessions.front().id.empty());
    // The reported ID is the one a front end can hand back to open_session.
    EXPECT_EQ(created_id, sessions.front().id);
    EXPECT_EQ(sessions.front().label, "Browser-ready session");
    EXPECT_TRUE(sessions.front().error.empty());

    OpenedSession reopened_session = workspace.open_session(
        {"lobby", sessions.front().id}, notifier());
    std::unique_ptr<SessionController> reopened = std::move(reopened_session.controller);
    EXPECT_TRUE(reopened->transcript().entries().empty());
    reopened->shutdown();
}

TEST_F(ApplicationWorkspaceTest, CreatesAStoredSessionWithoutOpeningIt) {
    {
        std::ofstream base_config(
            root_ / "forums" / "lobby" / "members" / "character_defaults.toml");
        base_config << "host = \"127.0.0.1\"\n"
                    << "port = 1\n"
                    << "mode = \"net\"\n";
    }
    Workspace workspace(root_);

    const SessionSummary created = workspace.create_stored_session("lobby", "");

    EXPECT_FALSE(created.id.empty());
    EXPECT_EQ(created.label, created.id);
    EXPECT_TRUE(created.error.empty());
    EXPECT_EQ(workspace.sessions("lobby"),
              (std::vector<SessionSummary>{created}));

    // A network-mode provider with no configured model would discover one
    // during controller construction. Creation succeeded without that work.
}

TEST_F(ApplicationWorkspaceTest, ReadsOneStoredSessionSummaryDirectly) {
    Workspace workspace(root_);
    const SessionSummary created =
        workspace.create_stored_session("lobby", "Selected session");

    EXPECT_EQ(
        workspace.session_summary("lobby", created.id),
        created);
    EXPECT_THROW(
        (void)workspace.session_summary("lobby", "missing"),
        SessionNotFoundError);
}

TEST_F(ApplicationWorkspaceTest, CreateStoredSessionValidatesBeforePublishing) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "members" / "character_defaults.toml");
    // guide/character.toml inherits host and port from workspace.toml after the
    // shared defaults are removed.
    Workspace workspace(root_);

    EXPECT_NO_THROW(
        (void)workspace.create_stored_session("lobby", "Valid with provider"));
    EXPECT_THROW(
        (void)workspace.create_stored_session("../missing", "Invalid forum"),
        std::runtime_error);
    EXPECT_EQ(workspace.sessions("lobby").size(), 1U);
}

TEST_F(ApplicationWorkspaceTest, OpensAStoredSessionInASeparateStep) {
    Workspace workspace(root_);
    const SessionSummary created =
        workspace.create_stored_session("lobby", "Stored first");

    OpenedSession opened_session = workspace.open_session(
        {"lobby", created.id}, notifier());
    std::unique_ptr<SessionController> opened = std::move(opened_session.controller);

    EXPECT_TRUE(opened->transcript().entries().empty());
    opened->shutdown();
}

#ifndef _WIN32
TEST_F(ApplicationWorkspaceTest, HoldsTheLeaseThroughExplicitShutdown) {
    Workspace workspace(root_);
    OpenedSession created =
        workspace.create_session("lobby", "Shutdown lease", notifier());
    const std::string session_id = created.descriptor.identity.session_id;
    created.controller->shutdown();
    created.controller.reset();

    OpenedSession opened = workspace.open_session({"lobby", session_id}, notifier());
    std::unique_ptr<SessionController> controller = std::move(opened.controller);
    const std::filesystem::path database =
        root_ / "forums" / "lobby" / "sessions"
        / (session_id + ".sqlite3");

    controller->shutdown();
    EXPECT_EQ(
        test::probe_lease(database),
        test::LeaseProbeResult::busy);

    controller.reset();
    EXPECT_EQ(
        test::probe_lease(database),
        test::LeaseProbeResult::acquired);
}

TEST_F(ApplicationWorkspaceTest, ReportsContentionBeforeRestoringSessionState) {
    Workspace workspace(root_);
    const SessionSummary created =
        workspace.create_stored_session("lobby", "Leased");
    const std::string session_id = created.id;
    const std::filesystem::path database =
        root_ / "forums" / "lobby" / "sessions" / (session_id + ".sqlite3");

    sqlite3* handle{};
    ASSERT_EQ(sqlite3_open_v2(
                  database.string().c_str(),
                  &handle,
                  SQLITE_OPEN_READWRITE,
                  nullptr),
              SQLITE_OK);
    // Keep a restorable-looking database whose next entry ID is invalid. If
    // open_session restores before acquiring the lease, unsigned_id() rejects
    // this value with std::runtime_error instead of the expected busy error.
    // Disabling CHECK constraints is what makes that ordering sentinel possible.
    ASSERT_EQ(sqlite3_exec(
                  handle,
                  "PRAGMA ignore_check_constraints = ON; "
                  "UPDATE state SET next_entry_id = 0 WHERE singleton = 1",
                  nullptr,
                  nullptr,
                  nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_close(handle), SQLITE_OK);

    test::LeaseHolderProcess holder(database);

    EXPECT_THROW(
        (void)workspace.open_session({"lobby", session_id}, notifier()),
        SessionBusyError);
    EXPECT_EQ(workspace.sessions("lobby"),
              (std::vector<SessionSummary>{created}));
}
#endif

TEST_F(ApplicationWorkspaceTest, SupportsAWorkspaceWithoutSharedCharacterConfig) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "members" / "character_defaults.toml");
    {
        std::ofstream config(
            root_ / "characters" / "guide" / "character.toml");
        config << "display_name = \"Guide\"\n"
               << "host = \"127.0.0.1\"\n"
               << "port = 8080\n";
    }
    Workspace workspace(root_);

    OpenedSession session =
        workspace.create_session(
            "lobby",
            "No shared config",
            notifier());

    session.controller->shutdown();
}

TEST_F(ApplicationWorkspaceTest, MapsInvalidStoredSessionDetails) {
    Workspace workspace(root_);
    OpenedSession created =
        workspace.create_session(
            "lobby",
            "Broken later",
            notifier());
    created.controller->shutdown();
    created.controller.reset();

    const std::vector<SessionSummary> healthy = workspace.sessions("lobby");
    ASSERT_EQ(healthy.size(), 1U);
    const std::string id = healthy.front().id;
    {
        std::ofstream database(
            root_ / "forums" / "lobby" / "sessions" / (id + ".sqlite3"),
            std::ios::binary | std::ios::trunc);
        database << "not SQLite";
    }

    const std::vector<SessionSummary> invalid = workspace.sessions("lobby");
    ASSERT_EQ(invalid.size(), 1U);
    EXPECT_EQ(invalid.front().id, id);
    EXPECT_EQ(invalid.front().label, id + " [invalid database]");
    EXPECT_FALSE(invalid.front().error.empty());
}

} // namespace
} // namespace cha
