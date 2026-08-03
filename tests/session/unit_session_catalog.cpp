#include "transcript/transcript.h"
#include "session/session_database.h"
#include "session/session_catalog.h"
#include "session/workspace.h"
#include "support/test_session_database.h"
#include "support/test_transcript.h"
#include "util/utf8_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>

namespace cha {
namespace {

// Builds and cleans up a minimal workspace fixture for forum and session tests.
class WorkspaceTest : public testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path()
            / ("cha_workspace_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(
            root / "characters" / "guide");
        std::filesystem::create_directories(root / "forums" / "lobby" / "members" / "guide");
        std::filesystem::create_directories(root / "personas" / "reader");
        std::filesystem::create_directories(root / "forums" / "lobby" / "sessions");
        {
            std::ofstream file(root / "app.toml");
            file << "host = \"127.0.0.1\"\n"
                 << "port = 8080\n"
                 << "[logging]\n"
                 << "file = \"logs/cha.log\"\n"
                 << "level = \"off\"\n";
        }
        {
            std::ofstream file(root / "forums" / "lobby" / "config.toml");
            file << "display_name = \"The Lobby\"\n";
        }
        {
            std::ofstream file(
                root / "characters" / "guide" / "character.toml");
            file << "display_name = \"Guide\"\n"
                 << "host = \"127.0.0.1\"\nport = 8080\n";
        }
        {
            std::ofstream file(
                root / "characters" / "guide" / "CHARACTER.md");
            file << "Character instructions";
        }
        {
            std::ofstream file(root / "forums" / "lobby" / "FORUM.md");
            file << "Forum instructions";
        }
        std::ofstream(root / "personas" / "reader" / "persona.toml")
            << "display_name = \"Reader\"\n";
    }

    void TearDown() override {
        std::filesystem::remove_all(root);
    }

    std::filesystem::path create_database(
        std::string id,
        std::string label,
        std::string forum = "lobby") {

        const std::filesystem::path path =
            root / "forums" / "lobby" / "sessions" / (id + ".sqlite3");
        if (!create_session_database(
                path,
                {
                    .id = std::move(id),
                    .forum = std::move(forum),
                    .label = std::move(label),
                })) {
            throw std::runtime_error("Failed to create workspace test database");
        }
        return path;
    }

    std::filesystem::path root;
};

TranscriptEntry human(EntryId id, std::string text, RequestId request_id) {
    return test::human_entry(
        id, {"human", "You"}, {"guide-id", "Guide"}, std::move(text), request_id);
}

TEST_F(WorkspaceTest, LoadsForumsAndTheirCharacterDirectories) {
    Workspace workspace(root);

    EXPECT_EQ(workspace.forums(), (std::vector<std::string>{"lobby"}));
    const Forum forum = workspace.load_forum("lobby");

    EXPECT_EQ(forum.name, "lobby");
    EXPECT_EQ(forum.display_name, "The Lobby");
    EXPECT_EQ(forum.character_names, (std::vector<std::string>{"guide"}));
}

TEST_F(WorkspaceTest, EnumeratesForumSubdirectoriesInNameOrder) {
    const std::filesystem::path alpha = root / "forums" / "alpha";
    std::filesystem::create_directories(alpha / "members" / "guide");
    std::ofstream(alpha / "config.toml") << "display_name = \"Alpha\"\n";
    std::ofstream(alpha / "FORUM.md") << "Alpha";
    std::ofstream(root / "forums" / "README.md") << "not a forum";

    Workspace workspace(root);

    EXPECT_EQ(
        workspace.forums(),
        (std::vector<std::string>{"alpha", "lobby"}));
}

TEST_F(WorkspaceTest, RejectsForumDirectoryNamesThatAreNotUrlSafe) {
    std::filesystem::create_directories(root / "forums" / "bad#fragment");
    std::filesystem::create_directories(root / "forums" / "bad%escape");
    std::filesystem::create_directories(root / "forums" / "bad space");
    Workspace workspace(root);

    EXPECT_EQ(workspace.forums(), (std::vector<std::string>{"lobby"}));
    EXPECT_THROW((void)workspace.load_forum("bad#fragment"), std::runtime_error);
    EXPECT_THROW((void)workspace.load_forum("bad%escape"), std::runtime_error);
    EXPECT_THROW((void)workspace.load_forum("bad space"), std::runtime_error);
}

TEST_F(WorkspaceTest, AllowsTheForumDirectoryToBeTemporarilyEmpty) {
    std::filesystem::remove_all(root / "forums" / "lobby");
    Workspace workspace(root);
    EXPECT_TRUE(workspace.forums().empty());
}

TEST_F(WorkspaceTest, ListsSessionDatabasesAndReturnsTheirPaths) {
    const std::filesystem::path saved =
        create_database("saved", "Saved session");
    const TranscriptEntry prompt = human(1, "Hello", 1);
    {
        SessionJournal journal(saved);
        journal.start_turn(1, prompt);
        journal.cancel_turn(1, std::nullopt);
        std::ofstream unrelated(
            root / "forums" / "lobby" / "sessions" / "ignored.data");
        unrelated << "not a session database";
    }

    Workspace workspace(root);
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(forum.directory / "sessions", forum.name);
    EXPECT_EQ(
        sessions.list(),
        (std::vector<Session>{{"saved", "Saved session"}}));
    EXPECT_EQ(sessions.session("saved"), (Session{"saved", "Saved session"}));
    EXPECT_THROW((void)sessions.session("missing"), SessionNotFoundError);
    EXPECT_EQ(
        load_transcript_entries(sessions.open_database_path("saved")),
        (std::vector<TranscriptEntry>{prompt}));
}

TEST_F(WorkspaceTest, RejectsSessionAndForumIdsThatAreNotUrlSafe) {
    (void)create_database("unsafe#fragment", "Unsafe session");
    const std::filesystem::path saved = create_database("saved", "Saved session");
    Workspace workspace(root);
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(forum.directory / "sessions", forum.name);

    EXPECT_EQ(sessions.list(), (std::vector<Session>{{"saved", "Saved session"}}));
    EXPECT_EQ(sessions.database_path("saved"), saved);
    EXPECT_THROW(
        (void)sessions.database_path("unsafe#fragment"),
        std::runtime_error);
    EXPECT_THROW(
        (void)SessionCatalog(forum.directory / "sessions", "bad?forum"),
        std::runtime_error);
}

TEST_F(WorkspaceTest, CreatesASelectableSelfContainedDatabaseImmediately) {
    Workspace workspace(root);
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(forum.directory / "sessions", forum.name);

    const Session session = sessions.create("A named session");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        forum.directory / "sessions" / (session.id + ".sqlite3")));
    EXPECT_EQ(sessions.list(), (std::vector<Session>{{session.id, "A named session"}}));

    const TranscriptEntry prompt = human(1, "Persist me", 1);
    {
        SessionJournal journal(sessions.database_path(session.id));
        journal.start_turn(1, prompt);
        journal.cancel_turn(1, std::nullopt);
    }
    EXPECT_EQ(sessions.list(), (std::vector<Session>{{session.id, "A named session"}}));
    EXPECT_EQ(
        load_transcript_entries(sessions.open_database_path(session.id)),
        (std::vector<TranscriptEntry>{prompt}));
}

TEST(SessionDatabase, CreatesAndReadsFromANonAsciiPath) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / path_from_utf8(
            "cha_session_na\xc3\xafve_\xe6\x9d\xb1\xe4\xba\xac_"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count()));
    std::filesystem::create_directory(directory);
    const std::filesystem::path path = directory / "session.sqlite3";

    ASSERT_TRUE(create_session_database(
        path,
        {
            .id = "session",
            .forum = "forum",
            .label = "Session",
        }));
    const SessionDatabaseMetadata metadata =
        read_session_database_metadata(path);
    EXPECT_EQ(metadata.id, "session");
    EXPECT_EQ(metadata.forum, "forum");
    EXPECT_EQ(metadata.label, "Session");
    std::filesystem::remove_all(directory);
}

TEST_F(WorkspaceTest, UsesALocalTimestampAsTheDefaultSessionLabelAndIdentifier) {
    Workspace workspace(root);
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(forum.directory / "sessions", forum.name);
    const Session session = sessions.create("");

    EXPECT_EQ(session.label, session.id);
    EXPECT_TRUE(std::regex_match(session.id, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-session")));
}

TEST_F(WorkspaceTest, ReportsInvalidDatabasesButOmitsInvalidSessionIds) {
    Workspace workspace(root);
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(forum.directory / "sessions", forum.name);
    const Session healthy = sessions.create("Healthy");
    const Session broken = sessions.create("Broken database");
    const std::filesystem::path malformed_name =
        forum.directory / "sessions" / "..sqlite3";
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
    ASSERT_EQ(listed.size(), 2U);
    const auto broken_result = std::find_if(listed.begin(), listed.end(), [&](const Session& candidate) {
        return candidate.id == broken.id;
    });
    const auto valid = std::find_if(listed.begin(), listed.end(), [&](const Session& candidate) {
        return candidate.id == healthy.id;
    });
    ASSERT_NE(broken_result, listed.end());
    ASSERT_NE(valid, listed.end());
    EXPECT_FALSE(broken_result->error.empty());
    EXPECT_TRUE(valid->error.empty());
}

TEST_F(WorkspaceTest, RejectsMismatchedSessionMetadataWhenOpening) {
    Workspace workspace(root);
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(forum.directory / "sessions", forum.name);
    create_database("wrong-forum", "Wrong forum", "hall");

    const std::vector<Session> listed = sessions.list();
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_FALSE(listed.front().error.empty());
    EXPECT_THROW(
        (void)sessions.open_database_path("wrong-forum"),
        std::runtime_error);
}

TEST_F(WorkspaceTest, DistinguishesAMissingSessionFromInvalidStorage) {
    Workspace workspace(root);
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(forum.directory / "sessions", forum.name);

    EXPECT_THROW(
        (void)sessions.open_database_path("missing"),
        SessionNotFoundError);
}

TEST_F(WorkspaceTest, EnforcesEverySessionMetadataIdentityField) {
    Workspace workspace(root);
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(forum.directory / "sessions", forum.name);

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
    const Forum forum = workspace.load_forum("lobby");
    SessionCatalog sessions(
        forum.directory / "sessions",
        forum.name,
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
         std::filesystem::directory_iterator(forum.directory / "sessions")) {
        EXPECT_FALSE(utf8_path(entry.path().filename()).starts_with("."));
    }
}

TEST_F(WorkspaceTest, LoadsForumCharactersFromSubdirectoriesInNameOrder) {
    const std::filesystem::path other = root / "characters" / "other";
    std::filesystem::create_directories(other);
    std::filesystem::create_directories(root / "forums" / "lobby" / "members" / "other");
    std::ofstream(other / "character.toml") << "display_name = \"Other\"\n";
    std::ofstream(other / "CHARACTER.md") << "Other";
    Workspace workspace(root);
    EXPECT_EQ(
        workspace.load_forum("lobby").character_names,
        (std::vector<std::string>{"guide", "other"}));
}

TEST_F(WorkspaceTest, IgnoresFilesWhenEnumeratingForumCharacters) {
    std::ofstream(root / "forums" / "lobby" / "members" / "README.md")
        << "not a character";
    Workspace workspace(root);
    EXPECT_EQ(
        workspace.load_forum("lobby").character_names,
        (std::vector<std::string>{"guide"}));
}

TEST_F(WorkspaceTest, RejectsAnEmptyOrAbsentForumCharactersDirectory) {
    std::filesystem::remove_all(root / "forums" / "lobby" / "members");
    std::filesystem::create_directories(root / "forums" / "lobby" / "members");
    EXPECT_THROW((void)Workspace(root), std::runtime_error);
}

TEST_F(WorkspaceTest, OpensAStoredSessionWhateverTheCurrentForumCharactersAre) {
    Workspace workspace(root);
    SessionCatalog sessions(
        workspace.load_forum("lobby").directory / "sessions", "lobby");
    const Session session = sessions.create("Two agents");
    {
        SessionJournal journal(sessions.database_path(session.id));
        journal.start_turn(
            1,
            test::human_entry(
                1, {"human", "You"}, {"other-id", "Other"}, "Question", 1));
        journal.complete_turn(
            1,
            make_agent_entry(
                2,
                "other-id",
                "Other",
                "Answer",
                EntryStatus::complete,
                1));
    }

    // The forum now contains completely different characters; the session is unaffected.
    std::filesystem::remove_all(root / "forums" / "lobby" / "members");
    std::filesystem::create_directories(root / "forums" / "lobby" / "members" / "guide");
    const Forum reduced = workspace.load_forum("lobby");
    SessionCatalog reopened(reduced.directory / "sessions", reduced.name);

    ASSERT_EQ(reopened.list().size(), 1U);
    EXPECT_TRUE(reopened.list().front().error.empty());
    EXPECT_EQ(
        load_transcript_entries(reopened.open_database_path(session.id)).size(),
        2U);
    EXPECT_EQ(read_session_database_metadata(
                  reopened.open_database_path(session.id)).forum,
              "lobby");
}

} // namespace
} // namespace cha
