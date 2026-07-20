#include "conversation.h"
#include "conversation_file.h"
#include "session_repository.h"
#include "workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>

namespace cha {
namespace {

// Builds and cleans up a minimal workspace fixture for room and session tests.
class WorkspaceTest : public testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path()
            / ("cha_workspace_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root / "personas" / "guide");
        std::filesystem::create_directories(root / "rooms" / "lobby" / "sessions");
        {
            std::ofstream file(root / "rooms" / "rooms.list");
            file << "# rooms\n\nlobby\n";
        }
        {
            std::ofstream file(root / "personas" / "guide" / "config.toml");
            file << "id = \"guide-id\"\nname = \"Guide\"\n"
                 << "host = \"127.0.0.1\"\nport = 8080\n";
        }
        {
            std::ofstream file(root / "personas" / "guide" / "SYSTEM.md");
            file << "Persona instructions";
        }
        {
            std::ofstream file(root / "rooms" / "lobby" / "personas.list");
            file << "guide\n";
        }
        {
            std::ofstream file(root / "rooms" / "lobby" / "USER.md");
            file << "Room instructions";
        }
    }

    void TearDown() override {
        std::filesystem::remove_all(root);
    }

    std::filesystem::path root;
};

TEST_F(WorkspaceTest, LoadsRoomsAndResolvesTheirPersonaDirectory) {
    Workspace workspace(root);

    EXPECT_EQ(workspace.rooms(), (std::vector<std::string>{"lobby"}));
    const Room room = workspace.load_room("lobby");

    EXPECT_EQ(room.name, "lobby");
    EXPECT_EQ(room.persona_name, "guide");
    EXPECT_EQ(workspace.persona_directory(room), root / "personas" / "guide");
}

TEST_F(WorkspaceTest, ListsOnlyCompleteSessionPairsAndReturnsTheirDataPath) {
    Conversation conversation;
    conversation.add_entry(make_human_entry(1, "Hello"));
    save_conversation_file(root / "rooms" / "lobby" / "sessions" / "saved.data", conversation);
    {
        std::ofstream meta(root / "rooms" / "lobby" / "sessions" / "saved.meta");
        meta << "version = 1\n"
             << "room = \"lobby\"\n"
             << "persona = \"guide\"\n"
             << "label = \"saved\"\n";
        std::ofstream orphan(root / "rooms" / "lobby" / "sessions" / "orphan.data");
        orphan << "ignored";
    }

    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name, room.persona_name);
    EXPECT_EQ(sessions.list(), (std::vector<Session>{{"saved", "saved"}}));
    EXPECT_EQ(load_conversation_file(sessions.open_data_path("saved")), conversation.entries());
}

TEST_F(WorkspaceTest, CreatesSelectableSessionFilesImmediately) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name, room.persona_name);

    const Session session = sessions.create("A named session");
    EXPECT_TRUE(std::filesystem::is_regular_file(room.directory / "sessions" / (session.id + ".meta")));
    EXPECT_TRUE(std::filesystem::is_regular_file(room.directory / "sessions" / (session.id + ".data")));
    EXPECT_EQ(sessions.list(), (std::vector<Session>{{session.id, "A named session"}}));

    Conversation conversation;
    conversation.add_entry(make_human_entry(1, "Persist me"));
    save_conversation_file(room.directory / "sessions" / (session.id + ".data"), conversation);
    EXPECT_EQ(sessions.list(), (std::vector<Session>{{session.id, "A named session"}}));
}

TEST_F(WorkspaceTest, UsesALocalTimestampAsTheDefaultSessionLabelAndIdentifier) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name, room.persona_name);
    const Session session = sessions.create("");

    EXPECT_EQ(session.label, session.id);
    EXPECT_TRUE(std::regex_match(session.id, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-session")));
}

TEST_F(WorkspaceTest, ReportsInvalidMetadataWithoutHidingHealthySessions) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name, room.persona_name);
    const Session healthy = sessions.create("Healthy");
    const Session session = sessions.create("Broken metadata");
    {
        std::ofstream meta(room.directory / "sessions" / (session.id + ".meta"), std::ios::trunc);
        meta << "label = \"unterminated";
    }

    const std::vector<Session> listed = sessions.list();
    ASSERT_EQ(listed.size(), 2U);
    const auto broken = std::find_if(listed.begin(), listed.end(), [&](const Session& candidate) {
        return candidate.id == session.id;
    });
    const auto valid = std::find_if(listed.begin(), listed.end(), [&](const Session& candidate) {
        return candidate.id == healthy.id;
    });
    ASSERT_NE(broken, listed.end());
    ASSERT_NE(valid, listed.end());
    EXPECT_FALSE(broken->error.empty());
    EXPECT_TRUE(valid->error.empty());
}

TEST_F(WorkspaceTest, RejectsMismatchedSessionMetadataWhenOpening) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name, room.persona_name);
    const Session session = sessions.create("Wrong room");
    {
        std::ofstream meta(room.directory / "sessions" / (session.id + ".meta"), std::ios::trunc);
        meta << "version = 1\n"
             << "room = \"hall\"\n"
             << "persona = \"guide\"\n"
             << "label = \"Wrong room\"\n";
    }

    const std::vector<Session> listed = sessions.list();
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_FALSE(listed.front().error.empty());
    EXPECT_THROW((void)sessions.open_data_path(session.id), std::runtime_error);
}

TEST_F(WorkspaceTest, EnforcesEverySessionMetadataIdentityField) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name, room.persona_name);

    const Session unsupported = sessions.create("Unsupported");
    {
        std::ofstream meta(
            room.directory / "sessions" / (unsupported.id + ".meta"),
            std::ios::trunc);
        meta << "version = 2\n"
             << "room = \"lobby\"\n"
             << "persona = \"guide\"\n"
             << "label = \"Unsupported\"\n";
    }

    const Session wrong_persona = sessions.create("Wrong persona");
    {
        std::ofstream meta(
            room.directory / "sessions" / (wrong_persona.id + ".meta"),
            std::ios::trunc);
        meta << "version = 1\n"
             << "room = \"lobby\"\n"
             << "persona = \"other\"\n"
             << "label = \"Wrong persona\"\n";
    }

    const Session incomplete = sessions.create("Incomplete");
    {
        std::ofstream meta(
            room.directory / "sessions" / (incomplete.id + ".meta"),
            std::ios::trunc);
        meta << "version = 1\n"
             << "persona = \"guide\"\n"
             << "label = \"Incomplete\"\n";
    }

    EXPECT_THROW((void)sessions.open_data_path(unsupported.id), std::runtime_error);
    EXPECT_THROW((void)sessions.open_data_path(wrong_persona.id), std::runtime_error);
    EXPECT_THROW((void)sessions.open_data_path(incomplete.id), std::runtime_error);
    const std::vector<Session> listed = sessions.list();
    ASSERT_EQ(listed.size(), 3U);
    for (const Session& session : listed) {
        EXPECT_FALSE(session.error.empty());
    }
}

TEST_F(WorkspaceTest, DoesNotAdoptAnOrphanedConversationFile) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name, room.persona_name);
    const Session orphan = sessions.create("Orphan");
    const std::filesystem::path orphan_meta =
        room.directory / "sessions" / (orphan.id + ".meta");
    const std::filesystem::path orphan_data =
        room.directory / "sessions" / (orphan.id + ".data");
    std::filesystem::remove(orphan_meta);

    const Session created = sessions.create("Replacement");

    EXPECT_NE(created.id, orphan.id);
    EXPECT_TRUE(std::filesystem::is_regular_file(orphan_data));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        room.directory / "sessions" / (created.id + ".meta")));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        room.directory / "sessions" / (created.id + ".data")));
}

TEST_F(WorkspaceTest, RejectsRoomsWithMoreThanOnePersona) {
    {
        std::ofstream file(root / "rooms" / "lobby" / "personas.list", std::ios::app);
        file << "another\n";
    }
    Workspace workspace(root);
    EXPECT_THROW({
        const Room room = workspace.load_room("lobby");
        (void)room;
    }, std::runtime_error);
}

} // namespace
} // namespace cha
