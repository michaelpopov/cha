#include "session/session_controller.h"
#include "session/workspace.h"
#include "support/test_notifier.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

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
        std::filesystem::create_directories(
            root_ / "forums" / "lobby" / "personas" / "guide");
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
                root_ / "forums" / "lobby" / "USER.md");
            forum_prompt << "Forum instructions";
        }
        {
            std::ofstream base_config(
                root_ / "forums" / "lobby" / "personas" / "persona_defaults.toml");
            base_config << "host = \"127.0.0.1\"\n"
                        << "port = 8080\n";
        }
        {
            std::ofstream config(
                root_ / "forums" / "lobby" / "personas" / "guide" / "persona.toml");
            config << "display_name = \"Guide\"\n";
        }
        {
            std::ofstream system_prompt(
                root_ / "forums" / "lobby" / "personas" / "guide" / "SYSTEM.md");
            system_prompt << "Persona instructions";
        }
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
    EXPECT_EQ(forum.persona_names, (std::vector<std::string>{"guide"}));
    EXPECT_TRUE(workspace.sessions("lobby").empty());
}

TEST_F(ApplicationWorkspaceTest, ForumCheckRequiresEffectiveSettings) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "personas" / "persona_defaults.toml");
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
            root_ / "forums" / "lobby" / "personas" / "guide" / "SYSTEM.md");
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

TEST_F(ApplicationWorkspaceTest, ForumCheckRejectsDuplicatePersonaNames) {
    const std::filesystem::path duplicate =
        root_ / "forums" / "lobby" / "personas" / "other";
    std::filesystem::create_directories(duplicate);
    {
        std::ofstream config(duplicate / "persona.toml");
        config << "display_name = \"Guide\"\n";
        std::ofstream system_prompt(duplicate / "SYSTEM.md");
        system_prompt << "Other instructions";
    }
    Workspace workspace(root_);

    EXPECT_THROW((void)workspace.check_forum("lobby"), std::invalid_argument);
    EXPECT_TRUE(workspace.sessions("lobby").empty());
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

TEST_F(ApplicationWorkspaceTest, SupportsAWorkspaceWithoutSharedPersonaConfig) {
    std::filesystem::remove(
        root_ / "forums" / "lobby" / "personas" / "persona_defaults.toml");
    {
        std::ofstream config(
            root_ / "forums" / "lobby" / "personas" / "guide" / "persona.toml");
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
