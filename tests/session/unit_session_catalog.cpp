#include "transcript/transcript.h"
#include "session/session_database.h"
#include "session/session_catalog.h"
#include "session/catalog_lease.h"
#include "session/session_lease.h"
#include "support/lease_test_process.h"
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

// Builds and cleans up a minimal workspace fixture for session storage tests.
class SessionStorageTest : public testing::Test {
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
            std::ofstream file(root / "workspace.toml");
            file << "host = \"127.0.0.1\"\n"
                 << "port = 8080\n"
                 << "[provider]\n"
                 << "host = \"test\"\nport = 1\nmode = \"test\"\n"
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

    // Session tests address storage by explicit fixture path; resolving the
    // workspace layout is WorkspaceModel's job, not this catalog's.
    std::filesystem::path sessions_directory(std::string_view forum = "lobby") const {
        return root / "forums" / std::string(forum) / "sessions";
    }

    std::filesystem::path root;
};

TranscriptEntry human(EntryId id, std::string text, RequestId request_id) {
    return test::human_entry(
        id, {"human", "You"}, {"guide-id", "Guide"}, std::move(text), request_id);
}

TEST_F(SessionStorageTest, ListsSessionDatabasesAndReturnsTheirPaths) {
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

    SessionCatalog sessions(sessions_directory(), "lobby");
    EXPECT_EQ(
        sessions.list(),
        (std::vector<Session>{{"saved", "Saved session"}}));
    EXPECT_EQ(sessions.session("saved"), (Session{"saved", "Saved session"}));
    EXPECT_THROW((void)sessions.session("missing"), SessionNotFoundError);
    EXPECT_EQ(
        load_transcript_entries(sessions.open_database_path("saved")),
        (std::vector<TranscriptEntry>{prompt}));
}

TEST_F(SessionStorageTest, RejectsSessionAndForumIdsThatAreNotUrlSafe) {
    (void)create_database("unsafe#fragment", "Unsafe session");
    const std::filesystem::path saved = create_database("saved", "Saved session");
    SessionCatalog sessions(sessions_directory(), "lobby");

    EXPECT_EQ(sessions.list(), (std::vector<Session>{{"saved", "Saved session"}}));
    EXPECT_EQ(sessions.database_path("saved"), saved);
    EXPECT_THROW(
        (void)sessions.database_path("unsafe#fragment"),
        std::runtime_error);
    EXPECT_THROW(
        (void)SessionCatalog(sessions_directory(), "bad?forum"),
        std::runtime_error);
}

TEST_F(SessionStorageTest, CreatesASelectableSelfContainedDatabaseImmediately) {
    SessionCatalog sessions(sessions_directory(), "lobby");

    const Session session = sessions.create("A named session");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions_directory() / (session.id + ".sqlite3")));
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

TEST_F(SessionStorageTest, ListingNeverIncludesLockFiles) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    CatalogLease catalog_lease = CatalogLease::acquire(sessions_directory());
    SessionLease session_lease = SessionLease::acquire(sessions.database_path("unpublished"));

    EXPECT_TRUE(sessions.list().empty());
}

TEST_F(SessionStorageTest, DefaultCreatesUseDistinctStemsUnderAFixedClock) {
    SessionCatalog sessions(
        sessions_directory(), "lobby", [] { return std::time_t{1234567890}; });
    const Session first = sessions.create("");
    const Session second = sessions.create("");

    EXPECT_EQ(first.label, first.id);
    EXPECT_EQ(second.label, second.id);
    EXPECT_NE(first.id, second.id);
}

TEST_F(SessionStorageTest, CreateSkipsABusyUnpublishedStem) {
    SessionCatalog sessions(
        sessions_directory(), "lobby",
        [] { return std::time_t{1234567890}; });
    const Session candidate = sessions.create("Temporary candidate");
    const std::filesystem::path candidate_path =
        sessions.database_path(candidate.id);
    SessionLease held_lease = SessionLease::acquire(candidate_path);
    ASSERT_TRUE(std::filesystem::remove(candidate_path));

    const Session created = sessions.create("Morning discussion");

    EXPECT_EQ(created.id, candidate.id + "-2");
    EXPECT_EQ(created.label, "Morning discussion");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions.database_path(created.id)));
}

TEST_F(SessionStorageTest, ProcessCatalogContentionIsBoundedAndCreatorDeathReleasesIt) {
    const std::filesystem::path directory = sessions_directory();
    test::CatalogHolderProcess holder(directory);

    EXPECT_EQ(test::create_catalog_session(directory, "lobby", "Morning"), test::CatalogCreateResult::busy);
    holder.terminate();
    EXPECT_EQ(test::create_catalog_session(directory, "lobby", "Morning"), test::CatalogCreateResult::succeeded);
}

TEST_F(SessionStorageTest, SameLabelIsAllowedInDifferentForums) {
    const std::filesystem::path alpha = root / "forums" / "alpha";
    std::filesystem::create_directories(alpha / "members" / "guide");
    std::ofstream(alpha / "config.toml") << "display_name = \"Alpha\"\n";
    std::ofstream(alpha / "FORUM.md") << "Alpha";
    const Session lobby_session =
        SessionCatalog(sessions_directory(), "lobby").create("Shared");
    const Session other_session =
        SessionCatalog(sessions_directory("alpha"), "alpha").create("Shared");
    EXPECT_EQ(lobby_session.label, "Shared");
    EXPECT_EQ(other_session.label, "Shared");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions_directory() / (lobby_session.id + ".sqlite3")));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions_directory("alpha") / (other_session.id + ".sqlite3")));
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

TEST_F(SessionStorageTest, UsesALocalTimestampAsTheDefaultSessionLabelAndIdentifier) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    const Session session = sessions.create("");

    EXPECT_EQ(session.label, session.id);
    EXPECT_TRUE(std::regex_match(session.id, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-session")));
}

TEST_F(SessionStorageTest, ReportsInvalidDatabasesButOmitsInvalidSessionIds) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    const Session healthy = sessions.create("Healthy");
    const Session broken = sessions.create("Broken database");
    const std::filesystem::path malformed_name =
        sessions_directory() / "..sqlite3";
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

TEST_F(SessionStorageTest, RejectsMismatchedSessionMetadataWhenOpening) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    create_database("wrong-forum", "Wrong forum", "hall");

    const std::vector<Session> listed = sessions.list();
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_FALSE(listed.front().error.empty());
    EXPECT_THROW(
        (void)sessions.open_database_path("wrong-forum"),
        std::runtime_error);
}

TEST_F(SessionStorageTest, DistinguishesAMissingSessionFromInvalidStorage) {
    SessionCatalog sessions(sessions_directory(), "lobby");

    EXPECT_THROW(
        (void)sessions.open_database_path("missing"),
        SessionNotFoundError);
}

TEST_F(SessionStorageTest, EnforcesEverySessionMetadataIdentityField) {
    SessionCatalog sessions(sessions_directory(), "lobby");

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

TEST_F(SessionStorageTest, CreatesDistinctDatabasesOnTimestampCollision) {
    SessionCatalog sessions(
        sessions_directory(),
        "lobby",
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
         std::filesystem::directory_iterator(sessions_directory())) {
        EXPECT_FALSE(utf8_path(entry.path().filename()).starts_with("."));
    }
}

TEST_F(SessionStorageTest, OpensAStoredSessionWhateverTheCurrentForumCharactersAre) {
    SessionCatalog sessions(sessions_directory(), "lobby");
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
    SessionCatalog reopened(sessions_directory(), "lobby");

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
