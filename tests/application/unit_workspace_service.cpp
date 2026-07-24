#include "application/chat_coordinator.h"
#include "application/workspace_service.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace cha {
namespace {

class WorkspaceServiceTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_workspace_service_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "personas" / "guide");
        std::filesystem::create_directories(root_ / "rooms" / "lobby");
        {
            std::ofstream rooms(root_ / "rooms" / "rooms.list");
            rooms << "lobby\n";
        }
        {
            std::ofstream personas(
                root_ / "rooms" / "lobby" / "personas.list");
            personas << "guide\n";
        }
        {
            std::ofstream room_prompt(
                root_ / "rooms" / "lobby" / "USER.md");
            room_prompt << "Room instructions";
        }
        {
            std::ofstream config(
                root_ / "personas" / "guide" / "config.toml");
            config << "id = \"guide-id\"\n"
                   << "name = \"Guide\"\n"
                   << "host = \"127.0.0.1\"\n"
                   << "port = 8080\n";
        }
        {
            std::ofstream system_prompt(
                root_ / "personas" / "guide" / "SYSTEM.md");
            system_prompt << "Persona instructions";
        }
    }

    void TearDown() override {
        std::filesystem::remove_all(root_);
    }

    std::filesystem::path root_;
};

TEST_F(WorkspaceServiceTest, ListsRoomsAndSessionsAsApplicationValues) {
    WorkspaceService workspace(root_);

    EXPECT_EQ(workspace.rooms(), (std::vector<std::string>{"lobby"}));
    PreparedRoom room = workspace.prepare_room("lobby");
    EXPECT_TRUE(room.sessions().empty());
}

TEST_F(WorkspaceServiceTest, CreatesAndReopensAChatSession) {
    WorkspaceService workspace(root_);
    PreparedRoom room = workspace.prepare_room("lobby");

    std::unique_ptr<ChatCoordinator> created =
        room.create_session("Browser-ready session");
    created->shutdown();
    created.reset();

    const std::vector<SessionSummary> sessions =
        room.sessions();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_FALSE(sessions.front().id.empty());
    EXPECT_EQ(sessions.front().label, "Browser-ready session");
    EXPECT_TRUE(sessions.front().error.empty());

    std::unique_ptr<ChatCoordinator> reopened =
        room.open_session(sessions.front().id);
    EXPECT_TRUE(reopened->conversation().entries().empty());
    reopened->shutdown();
}

TEST_F(WorkspaceServiceTest, PreparedRoomReusesLoadedAgentDefinitions) {
    WorkspaceService workspace(root_);
    PreparedRoom room = workspace.prepare_room("lobby");
    std::filesystem::remove(
        root_ / "personas" / "guide" / "config.toml");
    std::filesystem::remove(
        root_ / "personas" / "guide" / "SYSTEM.md");

    EXPECT_TRUE(room.sessions().empty());
    std::unique_ptr<ChatCoordinator> created =
        room.create_session("Uses cached definitions");
    created->shutdown();
}

TEST_F(WorkspaceServiceTest, MapsInvalidStoredSessionDetails) {
    WorkspaceService workspace(root_);
    PreparedRoom room = workspace.prepare_room("lobby");
    std::unique_ptr<ChatCoordinator> created =
        room.create_session("Broken later");
    created->shutdown();
    created.reset();

    const std::vector<SessionSummary> healthy = room.sessions();
    ASSERT_EQ(healthy.size(), 1U);
    const std::string id = healthy.front().id;
    {
        std::ofstream database(
            root_ / "rooms" / "lobby" / "sessions" / (id + ".sqlite3"),
            std::ios::binary | std::ios::trunc);
        database << "not SQLite";
    }

    const std::vector<SessionSummary> invalid = room.sessions();
    ASSERT_EQ(invalid.size(), 1U);
    EXPECT_EQ(invalid.front().id, id);
    EXPECT_EQ(invalid.front().label, id + " [invalid database]");
    EXPECT_FALSE(invalid.front().error.empty());
}

} // namespace
} // namespace cha
