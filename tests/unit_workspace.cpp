#include "conversation.h"
#include "conversation_file.h"
#include "workspace.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>

namespace cha {
namespace {

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
            file << "host = \"127.0.0.1\"\nport = 8080\n";
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

TEST_F(WorkspaceTest, LoadsRoomsAndComposesTheSelectedPersonaPrompt) {
    Workspace workspace(root);

    EXPECT_EQ(workspace.rooms(), (std::vector<std::string>{"lobby"}));
    const Room room = workspace.load_room("lobby");

    EXPECT_EQ(room.name, "lobby");
    EXPECT_EQ(room.persona_name, "guide");
    EXPECT_EQ(room.config.name, "guide");
    EXPECT_EQ(room.config.system_prompt, "Persona instructions\n\nRoom instructions");
}

TEST_F(WorkspaceTest, ListsOnlyCompleteSessionPairsAndReturnsTheirDataPath) {
    Conversation conversation;
    conversation.add_message("You", "Hello");
    save_conversation_file(root / "rooms" / "lobby" / "sessions" / "saved.data", conversation);
    {
        std::ofstream meta(root / "rooms" / "lobby" / "sessions" / "saved.meta");
        meta << "version = 1\n";
        std::ofstream orphan(root / "rooms" / "lobby" / "sessions" / "orphan.data");
        orphan << "ignored";
    }

    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    EXPECT_EQ(workspace.sessions(room), (std::vector<Session>{{"saved", "saved"}}));
    EXPECT_EQ(load_conversation_file(workspace.session_data_path(room, "saved")), conversation.messages());
}

TEST_F(WorkspaceTest, CreatesSessionMetadataBeforeItsConversationIsSaved) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");

    const Session session = workspace.create_session(room, "A named session");
    EXPECT_TRUE(std::filesystem::is_regular_file(room.directory / "sessions" / (session.id + ".meta")));
    EXPECT_FALSE(std::filesystem::exists(room.directory / "sessions" / (session.id + ".data")));
    EXPECT_TRUE(workspace.sessions(room).empty());

    Conversation conversation;
    conversation.add_message("You", "Persist me");
    save_conversation_file(room.directory / "sessions" / (session.id + ".data"), conversation);
    EXPECT_EQ(workspace.sessions(room), (std::vector<Session>{{session.id, "A named session"}}));
}

TEST_F(WorkspaceTest, UsesALocalTimestampAsTheDefaultSessionLabelAndIdentifier) {
    Workspace workspace(root);
    const Session session = workspace.create_session(workspace.load_room("lobby"), "");

    EXPECT_EQ(session.label, session.id);
    EXPECT_TRUE(std::regex_match(session.id, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-session")));
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
