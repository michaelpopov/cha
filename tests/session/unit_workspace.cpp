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
            std::ofstream app_config(root_ / "app.toml");
            app_config << "host = \"127.0.0.1\"\n"
                       << "port = 8080\n"
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
    EXPECT_EQ(workspace.app_config().host, "127.0.0.1");
    EXPECT_EQ(workspace.app_config().port, 8080);
    EXPECT_EQ(workspace.app_config().log_file, root_ / "logs" / "cha.log");
    EXPECT_EQ(workspace.app_config().log_level, "off");
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

    const Forum forum = Workspace(root_).load_forum("lobby");
    EXPECT_EQ(forum.character_names, (std::vector<std::string>{"alpha", "guide"}));
    EXPECT_EQ(forum.default_agent_id, "guide");
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

TEST_F(ApplicationWorkspaceTest, PersonaLoadingDoesNotChangeWorkspaceConstruction) {
    std::filesystem::remove_all(root_ / "personas");
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);

    std::filesystem::create_directories(root_ / "personas");
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, PersonaLoadingDoesNotSkipMalformedDirectories) {
    std::filesystem::create_directories(root_ / "personas" / "missing_config");
    EXPECT_THROW((void)Workspace(root_), std::runtime_error);
}

TEST_F(ApplicationWorkspaceTest, RequiresApplicationConfiguration) {
    std::filesystem::remove(root_ / "app.toml");

    try {
        (void)Workspace(root_);
        FAIL() << "expected missing application config to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("app.toml"),
            std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, RequiresValidWebListenerConfiguration) {
    {
        std::ofstream app_config(root_ / "app.toml");
        app_config << "port = 8080\n"
                   << "[logging]\n"
                   << "file = \"logs/cha.log\"\n"
                   << "level = \"off\"\n";
    }
    EXPECT_THROW((void)load_application_config(root_), std::runtime_error);

    {
        std::ofstream app_config(root_ / "app.toml");
        app_config << "host = \"127.0.0.1\"\n"
                   << "port = 65536\n"
                   << "[logging]\n"
                   << "file = \"logs/cha.log\"\n"
                   << "level = \"off\"\n";
    }
    EXPECT_THROW((void)load_application_config(root_), std::runtime_error);
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

TEST_F(ApplicationWorkspaceTest, ForumCheckRequiresEffectiveSettings) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "members" / "character_defaults.toml");
    Workspace workspace(root_);

    try {
        (void)workspace.check_forum("lobby");
        FAIL() << "expected missing effective settings to fail";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("guide"), std::string::npos) << message;
        EXPECT_NE(message.find("host"), std::string::npos) << message;
        EXPECT_NE(message.find("port"), std::string::npos) << message;
    }
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

TEST_F(ApplicationWorkspaceTest, ForumCheckRejectsDuplicateCharacterNames) {
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
        EXPECT_NE(std::string(error.what()).find("guide"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("other"), std::string::npos);
    }
}

TEST_F(ApplicationWorkspaceTest, OpenSessionRejectsAPersonaCharacterIdCollision) {
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

TEST_F(ApplicationWorkspaceTest, OpenSessionRejectsAPersonaCharacterDisplayNameCollision) {
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

    CreatedSession created =
        workspace.create_session(
            "lobby",
            "Browser-ready session",
            notifier());
    const std::string created_id = created.id;
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

    std::unique_ptr<SessionController> reopened =
        workspace.open_session(
            "lobby",
            sessions.front().id,
            notifier());
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
    // guide/character.toml supplies only display_name, so removing the shared
    // defaults leaves the character without the required host or port.
    Workspace workspace(root_);

    EXPECT_THROW(
        (void)workspace.create_stored_session("lobby", "Invalid forum"),
        std::runtime_error);
    EXPECT_THROW(
        (void)workspace.create_stored_session("../missing", "Invalid forum"),
        std::runtime_error);
    EXPECT_TRUE(workspace.sessions("lobby").empty());
}

TEST_F(ApplicationWorkspaceTest, OpensAStoredSessionInASeparateStep) {
    Workspace workspace(root_);
    const SessionSummary created =
        workspace.create_stored_session("lobby", "Stored first");

    std::unique_ptr<SessionController> opened = workspace.open_session(
        "lobby", created.id, notifier());

    EXPECT_TRUE(opened->transcript().entries().empty());
    opened->shutdown();
}

#ifndef _WIN32
TEST_F(ApplicationWorkspaceTest, HoldsTheLeaseThroughExplicitShutdown) {
    Workspace workspace(root_);
    CreatedSession created =
        workspace.create_session("lobby", "Shutdown lease", notifier());
    const std::string session_id = created.id;
    created.controller->shutdown();
    created.controller.reset();

    std::unique_ptr<SessionController> controller =
        workspace.open_session("lobby", session_id, notifier());
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
        (void)workspace.open_session("lobby", session_id, notifier()),
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

    CreatedSession session =
        workspace.create_session(
            "lobby",
            "No shared config",
            notifier());

    session.controller->shutdown();
}

TEST_F(ApplicationWorkspaceTest, MapsInvalidStoredSessionDetails) {
    Workspace workspace(root_);
    CreatedSession created =
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
