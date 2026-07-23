#include "conversation.h"
#include "session_database.h"
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
        std::filesystem::create_directories(root / "personas" / "other");
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
            std::ofstream file(root / "personas" / "other" / "config.toml");
            file << "id = \"other-id\"\nname = \"Other\"\n"
                 << "host = \"127.0.0.1\"\nport = 8081\n";
        }
        {
            std::ofstream file(root / "personas" / "other" / "SYSTEM.md");
            file << "Other instructions";
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

    std::filesystem::path create_database(
        std::string id,
        std::string label,
        std::string room = "lobby") {

        const std::filesystem::path path =
            root / "rooms" / "lobby" / "sessions" / (id + ".sqlite3");
        if (!create_session_database(
                path,
                {
                    .id = std::move(id),
                    .room = std::move(room),
                    .label = std::move(label),
                })) {
            throw std::runtime_error("Failed to create workspace test database");
        }
        return path;
    }

    std::filesystem::path root;
};

ConversationEntry human(EntryId id, std::string text) {
    return make_human_entry(id, "guide-id", "Guide", std::move(text));
}

TEST_F(WorkspaceTest, LoadsRoomsAndResolvesTheirPersonaDirectory) {
    Workspace workspace(root);

    EXPECT_EQ(workspace.rooms(), (std::vector<std::string>{"lobby"}));
    const Room room = workspace.load_room("lobby");

    EXPECT_EQ(room.name, "lobby");
    EXPECT_EQ(room.persona_names, (std::vector<std::string>{"guide"}));
    EXPECT_EQ(workspace.persona_directory("guide"), root / "personas" / "guide");
}

TEST_F(WorkspaceTest, ListsSessionDatabasesAndReturnsTheirPaths) {
    const std::filesystem::path saved =
        create_database("saved", "Saved session");
    {
        ConversationJournal journal(saved);
        journal.append(human(1, "Hello"));
        std::ofstream unrelated(
            root / "rooms" / "lobby" / "sessions" / "ignored.data");
        unrelated << "not a session database";
    }

    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name);
    EXPECT_EQ(
        sessions.list(),
        (std::vector<Session>{{"saved", "Saved session"}}));
    EXPECT_EQ(
        load_conversation_entries(sessions.open_database_path("saved")),
        (std::vector<ConversationEntry>{human(1, "Hello")}));
}

TEST_F(WorkspaceTest, CreatesASelectableSelfContainedDatabaseImmediately) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name);

    const Session session = sessions.create("A named session");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        room.directory / "sessions" / (session.id + ".sqlite3")));
    EXPECT_EQ(sessions.list(), (std::vector<Session>{{session.id, "A named session"}}));

    {
        ConversationJournal journal(sessions.database_path(session.id));
        journal.append(human(1, "Persist me"));
    }
    EXPECT_EQ(sessions.list(), (std::vector<Session>{{session.id, "A named session"}}));
    EXPECT_EQ(
        load_conversation_entries(sessions.open_database_path(session.id)),
        (std::vector<ConversationEntry>{human(1, "Persist me")}));
}

TEST_F(WorkspaceTest, UsesALocalTimestampAsTheDefaultSessionLabelAndIdentifier) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name);
    const Session session = sessions.create("");

    EXPECT_EQ(session.label, session.id);
    EXPECT_TRUE(std::regex_match(session.id, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-session")));
}

TEST_F(WorkspaceTest, ReportsAnInvalidDatabaseWithoutHidingHealthySessions) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name);
    const Session healthy = sessions.create("Healthy");
    const Session broken = sessions.create("Broken database");
    const std::filesystem::path malformed_name =
        room.directory / "sessions" / "..sqlite3";
    {
        std::ofstream database(
            sessions.database_path(broken.id),
            std::ios::binary | std::ios::trunc);
        database << "not SQLite";
    }
    {
        std::ofstream database(malformed_name);
        database << "invalid session filename";
    }

    const std::vector<Session> listed = sessions.list();
    ASSERT_EQ(listed.size(), 3U);
    const auto broken_result = std::find_if(listed.begin(), listed.end(), [&](const Session& candidate) {
        return candidate.id == broken.id;
    });
    const auto valid = std::find_if(listed.begin(), listed.end(), [&](const Session& candidate) {
        return candidate.id == healthy.id;
    });
    const auto malformed = std::find_if(
        listed.begin(),
        listed.end(),
        [](const Session& candidate) { return candidate.id == "."; });
    ASSERT_NE(broken_result, listed.end());
    ASSERT_NE(valid, listed.end());
    ASSERT_NE(malformed, listed.end());
    EXPECT_FALSE(broken_result->error.empty());
    EXPECT_FALSE(malformed->error.empty());
    EXPECT_TRUE(valid->error.empty());
}

TEST_F(WorkspaceTest, RejectsMismatchedSessionMetadataWhenOpening) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name);
    create_database("wrong-room", "Wrong room", "hall");

    const std::vector<Session> listed = sessions.list();
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_FALSE(listed.front().error.empty());
    EXPECT_THROW(
        (void)sessions.open_database_path("wrong-room"),
        std::runtime_error);
}

TEST_F(WorkspaceTest, EnforcesEverySessionMetadataIdentityField) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(room.directory / "sessions", room.name);

    create_database("wrong-filename", "Wrong filename");
    std::filesystem::rename(
        sessions.database_path("wrong-filename"),
        sessions.database_path("renamed"));
    {
        std::ofstream unsupported(
            sessions.database_path("unsupported"),
            std::ios::binary);
        unsupported << "unsupported database";
    }

    EXPECT_THROW((void)sessions.open_database_path("renamed"), std::runtime_error);
    EXPECT_THROW((void)sessions.open_database_path("unsupported"), std::runtime_error);
    const std::vector<Session> listed = sessions.list();
    ASSERT_EQ(listed.size(), 2U);
    for (const Session& session : listed) {
        EXPECT_FALSE(session.error.empty());
    }
}

TEST_F(WorkspaceTest, CreatesDistinctDatabasesOnTimestampCollision) {
    Workspace workspace(root);
    const Room room = workspace.load_room("lobby");
    SessionRepository sessions(
        room.directory / "sessions",
        room.name,
        [] { return std::time_t{1'700'000'000}; });
    const Session first = sessions.create("First");
    const Session second = sessions.create("Second");

    EXPECT_EQ(second.id, first.id + "-2");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions.database_path(first.id)));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions.database_path(second.id)));
    EXPECT_EQ(
        read_session_database_metadata(sessions.database_path(first.id)).label,
        "First");
    EXPECT_EQ(
        read_session_database_metadata(sessions.database_path(second.id)).label,
        "Second");
    for (const auto& entry :
         std::filesystem::directory_iterator(room.directory / "sessions")) {
        EXPECT_FALSE(entry.path().filename().string().starts_with("."));
    }
}

TEST_F(WorkspaceTest, LoadsRoomsWithMultiplePersonasInDeclaredOrder) {
    {
        std::ofstream file(root / "rooms" / "lobby" / "personas.list", std::ios::app);
        file << "other\n";
    }
    Workspace workspace(root);
    EXPECT_EQ(
        workspace.load_room("lobby").persona_names,
        (std::vector<std::string>{"guide", "other"}));
}

TEST_F(WorkspaceTest, IgnoresBlankLinesAndCommentsInThePersonaList) {
    {
        std::ofstream file(root / "rooms" / "lobby" / "personas.list", std::ios::trunc);
        file << "# the roster\n\n  other  \n\n# guide moved down\nguide\n";
    }
    Workspace workspace(root);
    EXPECT_EQ(
        workspace.load_room("lobby").persona_names,
        (std::vector<std::string>{"other", "guide"}));
}

TEST_F(WorkspaceTest, RejectsAPersonaListNamingOnePersonaTwice) {
    {
        std::ofstream file(root / "rooms" / "lobby" / "personas.list", std::ios::trunc);
        file << "guide\nother\nguide\n";
    }
    Workspace workspace(root);

    try {
        (void)workspace.load_room("lobby");
        FAIL() << "expected a duplicate persona diagnostic";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("personas.list"), std::string::npos) << message;
        EXPECT_NE(message.find("'guide' more than once"), std::string::npos) << message;
    }
}

TEST_F(WorkspaceTest, DefersMissingPersonaValidationUntilPersonaResolution) {
    {
        std::ofstream file(root / "rooms" / "lobby" / "personas.list", std::ios::trunc);
        file << "guide\nabsent\n";
    }
    Workspace workspace(root);

    EXPECT_EQ(
        workspace.load_room("lobby").persona_names,
        (std::vector<std::string>{"guide", "absent"}));
    EXPECT_THROW((void)workspace.persona_directory("absent"), std::runtime_error);
}

TEST_F(WorkspaceTest, RejectsAnEmptyOrAbsentPersonaList) {
    Workspace workspace(root);
    {
        std::ofstream file(root / "rooms" / "lobby" / "personas.list", std::ios::trunc);
        file << "# nobody lives here\n\n";
    }

    try {
        (void)workspace.load_room("lobby");
        FAIL() << "expected an empty persona list diagnostic";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("does not name a persona"), std::string::npos)
            << error.what();
    }

    std::filesystem::remove(root / "rooms" / "lobby" / "personas.list");
    EXPECT_THROW((void)workspace.load_room("lobby"), std::runtime_error);
}

TEST_F(WorkspaceTest, RejectsAPersonaNameThatEscapesThePersonaDirectory) {
    {
        std::ofstream file(root / "rooms" / "lobby" / "personas.list", std::ios::trunc);
        file << "../personas/guide\n";
    }
    Workspace workspace(root);

    EXPECT_THROW((void)workspace.load_room("lobby"), std::runtime_error);
}

TEST_F(WorkspaceTest, OpensAStoredSessionWhateverTheCurrentRosterIs) {
    Workspace workspace(root);
    SessionRepository sessions(
        workspace.load_room("lobby").directory / "sessions", "lobby");
    const Session session = sessions.create("Two agents");
    {
        ConversationJournal journal(sessions.database_path(session.id));
        journal.append(make_human_entry(1, "other-id", "Other", "Question"));
        journal.append(make_agent_entry(
            2, "other-id", "Other", "Answer", CompletionStatus::complete));
    }

    // The room now hosts a completely different roster; the session is unaffected.
    {
        std::ofstream file(root / "rooms" / "lobby" / "personas.list", std::ios::trunc);
        file << "guide\n";
    }
    const Room reduced = workspace.load_room("lobby");
    SessionRepository reopened(reduced.directory / "sessions", reduced.name);

    ASSERT_EQ(reopened.list().size(), 1U);
    EXPECT_TRUE(reopened.list().front().error.empty());
    EXPECT_EQ(
        load_conversation_entries(reopened.open_database_path(session.id)).size(),
        2U);
    EXPECT_EQ(read_session_database_metadata(
                  reopened.open_database_path(session.id)).room,
              "lobby");
}

} // namespace
} // namespace cha
