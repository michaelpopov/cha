#include "chat/transcript.h"
#include "session/session_database.h"
#include "session/session_catalog.h"
#include "session/session_lease.h"
#include "support/test_session_database.h"
#include "support/test_transcript.h"
#include "util/path_name.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include "support/lease_test_process.h"
#endif

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
    // workspace layout is Workspace's job, not this catalog's.
    std::filesystem::path sessions_directory(std::string_view forum = "lobby") const {
        return root / "forums" / std::string(forum) / "sessions";
    }

    std::filesystem::path root;
};

TranscriptEntry human(EntryId id, std::string text, RequestId request_id) {
    return test::human_entry(
        id, {"human", "You"}, {"guide-id", "Guide"}, std::move(text), request_id);
}

using IdAndLabel = std::pair<std::string, std::string>;

// A listed session also carries its path and modification time; what these
// expectations are about is which sessions were offered and under what name.
std::vector<IdAndLabel> ids_and_labels(const std::vector<StoredSession>& listed) {
    std::vector<IdAndLabel> result;
    for (const StoredSession& stored : listed) {
        result.push_back({stored.identity.session_id, stored.label});
    }
    return result;
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
        ids_and_labels(sessions.list()),
        (std::vector<IdAndLabel>{{"saved", "Saved session"}}));
    const StoredSession inspected = sessions.inspect("saved");
    EXPECT_EQ(inspected.identity, (FullSessionId{"lobby", "saved"}));
    EXPECT_EQ(inspected.label, "Saved session");
    EXPECT_EQ(inspected.database_path, saved);
    EXPECT_TRUE(inspected.error.empty());
    EXPECT_THROW((void)sessions.inspect("missing"), SessionNotFoundError);
    EXPECT_EQ(
        load_transcript_entries(sessions.inspect("saved").database_path),
        (std::vector<TranscriptEntry>{prompt}));
}

TEST_F(SessionStorageTest, RejectsSessionAndForumIdsThatAreNotUrlSafe) {
    (void)create_database("unsafe#fragment", "Unsafe session");
    const std::filesystem::path saved = create_database("saved", "Saved session");
    SessionCatalog sessions(sessions_directory(), "lobby");

    EXPECT_EQ(
        ids_and_labels(sessions.list()),
        (std::vector<IdAndLabel>{{"saved", "Saved session"}}));
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

    const StoredSession session = sessions.create("A named session");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions_directory() / (session.identity.session_id + ".sqlite3")));
    EXPECT_EQ(
        ids_and_labels(sessions.list()),
        (std::vector<IdAndLabel>{{session.identity.session_id, "A named session"}}));

    const TranscriptEntry prompt = human(1, "Persist me", 1);
    {
        SessionJournal journal(sessions.database_path(session.identity.session_id));
        journal.start_turn(1, prompt);
        journal.cancel_turn(1, std::nullopt);
    }
    EXPECT_EQ(
        ids_and_labels(sessions.list()),
        (std::vector<IdAndLabel>{{session.identity.session_id, "A named session"}}));
    EXPECT_EQ(
        load_transcript_entries(sessions.inspect(session.identity.session_id).database_path),
        (std::vector<TranscriptEntry>{prompt}));
}

TEST_F(SessionStorageTest, ReportsThePathAndTimestampItObserved) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    const StoredSession created = sessions.create("Timed");
    EXPECT_EQ(
        created.database_path,
        sessions.database_path(created.identity.session_id));
    EXPECT_EQ(
        created.updated_at,
        std::filesystem::last_write_time(created.database_path));
    EXPECT_TRUE(created.error.empty());

    {
        SessionJournal journal(created.database_path);
        journal.start_turn(1, human(1, "Later", 1));
        journal.cancel_turn(1, std::nullopt);
    }

    const std::vector<StoredSession> listed = sessions.list();
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_EQ(listed.front().database_path, created.database_path);
    EXPECT_EQ(
        listed.front().updated_at,
        std::filesystem::last_write_time(created.database_path));
    EXPECT_EQ(sessions.inspect(created.identity.session_id).updated_at,
        listed.front().updated_at);
}

TEST_F(SessionStorageTest, ListingNeverIncludesLockFiles) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    SessionLease session_lease = SessionLease::acquire(sessions.database_path("unpublished"));
    // A catalog lock left by an older release is an ordinary file here, and is
    // ignored for the same reason every non-database entry is.
    std::ofstream(sessions_directory() / "catalog.cha-lock") << "";

    EXPECT_TRUE(sessions.list().empty());
}

TEST_F(SessionStorageTest, DefaultCreatesUseDistinctStemsUnderAFixedClock) {
    SessionCatalog sessions(
        sessions_directory(), "lobby", [] { return std::time_t{1234567890}; });
    const StoredSession first = sessions.create("");
    const StoredSession second = sessions.create("");

    EXPECT_EQ(first.label, first.identity.session_id);
    EXPECT_EQ(second.label, second.identity.session_id);
    EXPECT_NE(first.identity.session_id, second.identity.session_id);
}

TEST_F(SessionStorageTest, CreateDoesNotReuseAnIdPresentInDeletedStorage) {
    SessionCatalog sessions(
        sessions_directory(), "lobby", [] { return std::time_t{1234567890}; });
    const StoredSession first = sessions.create("First");
    const std::filesystem::path archived =
        sessions.deleted_database_path(first.identity.session_id);
    std::filesystem::create_directories(archived.parent_path());
    std::filesystem::rename(first.database_path, archived);

    const StoredSession second = sessions.create("Second");

    EXPECT_EQ(second.identity.session_id, first.identity.session_id + "-2");
    EXPECT_TRUE(std::filesystem::exists(archived));
}

TEST_F(SessionStorageTest, CreateSkipsABusyUnpublishedStem) {
    SessionCatalog sessions(
        sessions_directory(), "lobby",
        [] { return std::time_t{1234567890}; });
    const StoredSession candidate = sessions.create("Temporary candidate");
    const std::filesystem::path candidate_path =
        sessions.database_path(candidate.identity.session_id);
    SessionLease held_lease = SessionLease::acquire(candidate_path);
    ASSERT_TRUE(std::filesystem::remove(candidate_path));

    const StoredSession created = sessions.create("Morning discussion");

    EXPECT_EQ(created.identity.session_id, candidate.identity.session_id + "-2");
    EXPECT_EQ(created.label, "Morning discussion");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions.database_path(created.identity.session_id)));
}

#ifndef _WIN32
TEST_F(SessionStorageTest, RacingProcessesPublishOneSessionEachFromTheSameBaseId) {
    const std::vector<std::string> labels{"Morning", "Afternoon", "Evening"};
    {
        // Every creator derives the same base ID and they are released
        // together, so the suffixes are settled by candidate leases and
        // atomic publication rather than by ordering.
        test::CatalogCreationRace race(
            sessions_directory(), "lobby", labels, std::time_t{1'700'000'000});
        race.run();
    }

    SessionCatalog sessions(sessions_directory(), "lobby");
    const std::vector<StoredSession> listed = sessions.list();
    ASSERT_EQ(listed.size(), labels.size());

    std::set<std::string> published_ids;
    std::vector<std::string> published_labels;
    for (const StoredSession& session : listed) {
        EXPECT_TRUE(session.error.empty()) << session.error;
        EXPECT_TRUE(published_ids.insert(session.identity.session_id).second);
        const SessionDatabaseMetadata metadata =
            read_session_database_metadata(sessions.database_path(session.identity.session_id));
        EXPECT_EQ(metadata.id, session.identity.session_id);
        EXPECT_EQ(metadata.forum, "lobby");
        EXPECT_EQ(metadata.label, session.label);
        published_labels.push_back(session.label);
    }
    // Which creator wins the unsuffixed ID is deliberately unspecified.
    std::sort(published_labels.begin(), published_labels.end());
    std::vector<std::string> expected_labels = labels;
    std::sort(expected_labels.begin(), expected_labels.end());
    EXPECT_EQ(published_labels, expected_labels);

    // The one shared base plus its suffixes, proving the collisions really
    // happened and were resolved rather than avoided.
    const std::string base = listed.front().identity.session_id;
    const std::vector<std::string> ids(published_ids.begin(), published_ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{base, base + "-2", base + "-3"}));
}
#endif

TEST_F(SessionStorageTest, SameLabelIsAllowedInDifferentForums) {
    const std::filesystem::path alpha = root / "forums" / "alpha";
    std::filesystem::create_directories(alpha / "members" / "guide");
    std::ofstream(alpha / "config.toml") << "display_name = \"Alpha\"\n";
    std::ofstream(alpha / "FORUM.md") << "Alpha";
    const StoredSession lobby_session =
        SessionCatalog(sessions_directory(), "lobby").create("Shared");
    const StoredSession other_session =
        SessionCatalog(sessions_directory("alpha"), "alpha").create("Shared");
    EXPECT_EQ(lobby_session.label, "Shared");
    EXPECT_EQ(other_session.label, "Shared");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions_directory() / (lobby_session.identity.session_id + ".sqlite3")));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions_directory("alpha") / (other_session.identity.session_id + ".sqlite3")));
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
    const StoredSession session = sessions.create("");

    EXPECT_EQ(session.label, session.identity.session_id);
    EXPECT_TRUE(std::regex_match(session.identity.session_id, std::regex("[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-session")));
}

TEST_F(SessionStorageTest, ReportsInvalidDatabasesButOmitsInvalidSessionIds) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    const StoredSession healthy = sessions.create("Healthy");
    const StoredSession broken = sessions.create("Broken database");
    const std::filesystem::path malformed_name =
        sessions_directory() / "..sqlite3";
    {
        std::ofstream database(
            sessions.database_path(broken.identity.session_id),
            std::ios::binary | std::ios::trunc);
        database << "not SQLite";
    }
    {
        std::ofstream database(malformed_name);
        database << "invalid session filename";
    }

    const std::vector<StoredSession> listed = sessions.list();
    ASSERT_EQ(listed.size(), 2U);
    const auto broken_result = std::find_if(listed.begin(), listed.end(), [&](const StoredSession& candidate) {
        return candidate.identity.session_id == broken.identity.session_id;
    });
    const auto valid = std::find_if(listed.begin(), listed.end(), [&](const StoredSession& candidate) {
        return candidate.identity.session_id == healthy.identity.session_id;
    });
    ASSERT_NE(broken_result, listed.end());
    ASSERT_NE(valid, listed.end());
    EXPECT_FALSE(broken_result->error.empty());
    EXPECT_TRUE(valid->error.empty());
    // An invalid row is still identifiable storage: it keeps its path and
    // timestamp alongside the fallback label.
    EXPECT_EQ(
        broken_result->database_path,
        sessions.database_path(broken.identity.session_id));
    EXPECT_EQ(
        broken_result->updated_at,
        std::filesystem::last_write_time(broken_result->database_path));
    EXPECT_EQ(
        broken_result->label,
        broken.identity.session_id + " [invalid database]");
}

TEST_F(SessionStorageTest, RejectsMismatchedSessionMetadataWhenOpening) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    create_database("wrong-forum", "Wrong forum", "hall");

    const std::vector<StoredSession> listed = sessions.list();
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_FALSE(listed.front().error.empty());
    EXPECT_THROW(
        (void)sessions.inspect("wrong-forum"),
        std::runtime_error);
}

TEST_F(SessionStorageTest, DistinguishesAMissingSessionFromInvalidStorage) {
    SessionCatalog sessions(sessions_directory(), "lobby");

    EXPECT_THROW(
        (void)sessions.inspect("missing"),
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

    EXPECT_THROW((void)sessions.inspect("renamed"), std::runtime_error);
    EXPECT_THROW((void)sessions.inspect("unsupported"), std::runtime_error);
    const std::vector<StoredSession> listed = sessions.list();
    ASSERT_EQ(listed.size(), 2U);
    for (const StoredSession& session : listed) {
        EXPECT_FALSE(session.error.empty());
    }
}

TEST_F(SessionStorageTest, CreatesDistinctDatabasesOnTimestampCollision) {
    SessionCatalog sessions(
        sessions_directory(),
        "lobby",
        [] { return std::time_t{1'700'000'000}; });
    const StoredSession first = sessions.create("First");
    const StoredSession second = sessions.create("Second");

    EXPECT_EQ(second.identity.session_id, first.identity.session_id + "-2");
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions.database_path(first.identity.session_id)));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        sessions.database_path(second.identity.session_id)));
    EXPECT_EQ(
        read_session_database_metadata(sessions.database_path(first.identity.session_id)).label,
        "First");
    EXPECT_EQ(
        read_session_database_metadata(sessions.database_path(second.identity.session_id)).label,
        "Second");
    for (const auto& entry :
         std::filesystem::directory_iterator(sessions_directory())) {
        EXPECT_FALSE(utf8_path(entry.path().filename()).starts_with("."));
    }
}

TEST_F(SessionStorageTest, OpensAStoredSessionWhateverTheCurrentForumRosterIs) {
    SessionCatalog sessions(sessions_directory(), "lobby");
    const StoredSession session = sessions.create("Two agents");
    {
        SessionJournal journal(sessions.database_path(session.identity.session_id));
        journal.start_turn(
            1,
            test::human_entry(
                1, {"human", "You"}, {"other-id", "Other"}, "Question", 1));
        journal.complete_turn(
            1,
            make_character_entry(
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
        load_transcript_entries(reopened.inspect(session.identity.session_id).database_path).size(),
        2U);
    EXPECT_EQ(read_session_database_metadata(
                  reopened.inspect(session.identity.session_id).database_path).forum,
              "lobby");
}

} // namespace
} // namespace cha
