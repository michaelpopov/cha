#include "workspace/session_open.h"

#include "workspace/builtins.h"
#include "session/not_found_error.h"
#include "session/session_lease.h"
#include "session/session_repository.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifndef _WIN32
#include "support/lease_test_process.h"
#endif

namespace cha {
namespace {

class SessionOpenTest : public ::testing::Test {
protected:
    WorkspaceDefinition load_model() const {
        return WorkspaceDefinition::load(fixture_.root(), load_workspace_config(fixture_.root()));
    }

    std::unique_ptr<SessionRepository> make_repository(const WorkspaceDefinition& model) const {
        return std::make_unique<SessionRepository>(
            model.session_directories(),
            TemporarySessionSeed{
                {std::string(entrance_id), std::string(welcome_id)},
                std::string(welcome_name)});
    }

    static SessionIdentity welcome() {
        return {std::string(entrance_id), std::string(welcome_id)};
    }

    test::TestWorkspace fixture_;
    test::NoopNotifier notifier_;
};

TEST_F(SessionOpenTest, OpensAStoredSessionWithTheLoadedForumDefinitions) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);

    EXPECT_EQ(opened.descriptor.identity, created.identity);
    EXPECT_EQ(opened.descriptor.forum_display_name, "The Lobby");
    EXPECT_EQ(opened.descriptor.session_label, "Stored");
    EXPECT_EQ(opened.descriptor.forum_default_character_id, "guide");
    EXPECT_EQ(opened.descriptor.forum_default_persona_id, guest_id);
    ASSERT_EQ(opened.controller->view().characters.size(), 1U);
    EXPECT_EQ(opened.controller->view().characters.front().id, "guide");
    EXPECT_EQ(opened.controller->view().characters.front().display_name, "Guide");
    EXPECT_EQ(opened.controller->view().default_character_id, "guide");
    EXPECT_TRUE(opened.controller->view().transcript.entries.empty());
    opened.controller->shutdown();
}

TEST_F(SessionOpenTest, OpensWelcomeThroughTheSamePathWithTheAssistantRoster) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);

    OpenedSession opened = open_session(model, *sessions, welcome(), notifier_);

    EXPECT_EQ(opened.descriptor.identity, welcome());
    EXPECT_EQ(opened.descriptor.forum_display_name, entrance_name);
    EXPECT_EQ(opened.descriptor.session_label, welcome_name);
    EXPECT_EQ(opened.descriptor.forum_default_character_id, assistant_id);
    EXPECT_EQ(opened.descriptor.forum_default_persona_id, guest_id);
    ASSERT_EQ(opened.controller->view().characters.size(), 1U);
    EXPECT_EQ(opened.controller->view().characters.front().id, assistant_id);
    EXPECT_EQ(opened.controller->view().default_character_id, assistant_id);
    opened.controller->shutdown();
}

TEST_F(SessionOpenTest, GivesEveryControllerTheModelsGuestInclusivePersonaRoster) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    OpenedSession stored = open_session(model, *sessions, created.identity, notifier_);
    OpenedSession entrance = open_session(model, *sessions, welcome(), notifier_);

    // The roster is observable through author resolution: Guest and the
    // workspace persona are known to both controllers, an outsider is not.
    for (const OpenedSession& opened : {std::cref(stored), std::cref(entrance)}) {
        SessionController& controller = *opened.controller;
        EXPECT_EQ(
            controller.submit_prompt("outsider", "Hello").notice.value_or(""),
            "Unknown persona ID 'outsider'");
        EXPECT_EQ(
            controller.submit_prompt(guest_id, "Hello").notice.value_or(""),
            "");
        EXPECT_NE(
            controller.submit_prompt("reader", "Hello").notice.value_or(""),
            "Unknown persona ID 'reader'");
    }
    stored.controller->shutdown();
    entrance.controller->shutdown();
}

TEST_F(SessionOpenTest, PropagatesMissingForumsSessionsAndLeaseContention) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    EXPECT_THROW(
        (void)open_session(model, *sessions, {"absent", created.identity.session_id}, notifier_),
        ForumNotFoundError);
    EXPECT_THROW(
        (void)open_session(model, *sessions, {"lobby", "absent"}, notifier_),
        SessionNotFoundError);

    OpenedSession held = open_session(model, *sessions, created.identity, notifier_);
    EXPECT_THROW(
        (void)open_session(model, *sessions, created.identity, notifier_),
        SessionBusyError);
    held.controller->shutdown();
}

TEST_F(SessionOpenTest, ReleasesTheLeaseWhenControllerConstructionFails) {
    fixture_.write_character_defaults(
        "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n"
        "api_key_env = \"CHA_TEST_UNSET_API_KEY\"\n");
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    EXPECT_THROW(
        (void)open_session(model, *sessions, created.identity, notifier_),
        std::runtime_error);

    // The failed open must not leave the session leased against the next one.
    EXPECT_NO_THROW((void)sessions->prepare(created.identity));
}

TEST_F(SessionOpenTest, OpensWithTheLoadedModelAfterTheWorkspaceChangesOnDisk) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    fixture_.write_character_config("display_name = \"Renamed\"\n");
    std::ofstream(fixture_.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"Renamed Lobby\"\n";

    {
        OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);
        EXPECT_EQ(opened.descriptor.forum_display_name, "The Lobby");
        EXPECT_EQ(opened.controller->view().characters.front().display_name, "Guide");
        opened.controller->shutdown();
    }

    const WorkspaceDefinition reloaded = load_model();
    const auto reloaded_sessions = make_repository(reloaded);
    OpenedSession fresh =
        open_session(reloaded, *reloaded_sessions, created.identity, notifier_);
    EXPECT_EQ(fresh.descriptor.forum_display_name, "Renamed Lobby");
    EXPECT_EQ(fresh.controller->view().characters.front().display_name, "Renamed");
    fresh.controller->shutdown();
}

TEST_F(SessionOpenTest, ReopensAStoredSessionWithTheSameDescriptor) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Browser-ready session");

    SessionDescriptor first;
    {
        OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);
        first = opened.descriptor;
        EXPECT_EQ(
            first,
            (SessionDescriptor{
                .identity = created.identity,
                .forum_display_name = "The Lobby",
                .session_label = "Browser-ready session",
                .forum_default_character_id = "guide",
                .forum_default_persona_id = std::string(guest_id),
            }));
        opened.controller->shutdown();
    }

    const std::vector<StoredSession> listed = sessions->list("lobby");
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_EQ(listed.front().identity, created.identity);

    OpenedSession reopened = open_session(model, *sessions, created.identity, notifier_);
    EXPECT_EQ(reopened.descriptor, first);
    EXPECT_TRUE(reopened.controller->view().transcript.entries.empty());
    reopened.controller->shutdown();
}

TEST_F(SessionOpenTest, OpensAWorkspaceWithoutSharedCharacterDefaults) {
    std::filesystem::remove(
        fixture_.root() / "forums" / "lobby" / "members" / "character_defaults.toml");
    fixture_.write_character_config(
        "display_name = \"Guide\"\nhost = \"127.0.0.1\"\nport = 8080\n");
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "No shared config");

    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);
    opened.controller->shutdown();
}

#ifndef _WIN32
TEST_F(SessionOpenTest, HoldsTheLeaseThroughExplicitShutdownUntilTheControllerIsDestroyed) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Shutdown lease");
    const std::filesystem::path database =
        fixture_.root() / "forums" / "lobby" / "sessions"
        / (created.identity.session_id + ".sqlite3");

    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);
    opened.controller->shutdown();
    EXPECT_EQ(test::probe_lease(database), test::LeaseProbeResult::busy);

    opened.controller.reset();
    EXPECT_EQ(test::probe_lease(database), test::LeaseProbeResult::acquired);
}

TEST_F(SessionOpenTest, ReportsContentionBeforeRestoringSessionState) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Leased");
    const std::filesystem::path database =
        fixture_.root() / "forums" / "lobby" / "sessions"
        / (created.identity.session_id + ".sqlite3");

    sqlite3* handle{};
    ASSERT_EQ(
        sqlite3_open_v2(database.string().c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr),
        SQLITE_OK);
    // Keep a restorable-looking database whose next entry ID is invalid. If
    // preparation restored before acquiring the lease, unsigned_id() would
    // reject this value with std::runtime_error instead of the busy error.
    // Disabling CHECK constraints is what makes that ordering sentinel possible.
    ASSERT_EQ(
        sqlite3_exec(
            handle,
            "PRAGMA ignore_check_constraints = ON; "
            "UPDATE state SET next_entry_id = 0 WHERE singleton = 1",
            nullptr, nullptr, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_close(handle), SQLITE_OK);

    test::LeaseHolderProcess holder(database);

    EXPECT_THROW(
        (void)open_session(model, *sessions, created.identity, notifier_),
        SessionBusyError);
    ASSERT_EQ(sessions->list("lobby").size(), 1U);
    EXPECT_EQ(sessions->list("lobby").front().identity, created.identity);
}
#endif

} // namespace
} // namespace cha
