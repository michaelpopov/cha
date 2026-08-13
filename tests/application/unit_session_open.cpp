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
    EXPECT_EQ(opened.controller->view().default_persona_id, guest_id);
    ASSERT_EQ(opened.controller->view().characters.size(), 1U);
    EXPECT_EQ(opened.controller->view().characters.front().id, "guide");
    EXPECT_EQ(opened.controller->view().characters.front().display_name, "Guide");
    EXPECT_EQ(opened.controller->view().default_character_id, "guide");
    EXPECT_TRUE(opened.controller->view().transcript.entries.empty());
    opened.controller->shutdown();
}

TEST_F(SessionOpenTest, PersistsAChangedForumDefaultCharacter) {
    const std::filesystem::path character = fixture_.root() / "characters" / "alpha";
    std::filesystem::create_directories(character);
    std::filesystem::create_directories(
        fixture_.root() / "forums" / "lobby" / "members" / "alpha");
    std::ofstream(character / "character.toml") << "display_name = \"Alpha\"\n";
    std::ofstream(character / "CHARACTER.md") << "Alpha instructions\n";
    std::ofstream(fixture_.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndescription = \"Where it starts\"\n"
           "default_character = \"guide\"\n";

    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");
    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);

    ASSERT_TRUE(opened.persist_default_character);
    opened.persist_default_character("alpha");
    opened.controller->shutdown();

    // The saved value reaches the next session of this same server run.
    const StoredSession next = sessions->create("lobby", "Next");
    OpenedSession later = open_session(model, *sessions, next.identity, notifier_);
    EXPECT_EQ(later.descriptor.forum_default_character_id, "alpha");
    EXPECT_EQ(later.controller->view().default_character_id, "alpha");
    later.controller->shutdown();

    // Saving rewrites the file, so the settings around it have to survive.
    const WorkspaceDefinition reloaded = load_model();
    ASSERT_NE(reloaded.find_forum("lobby"), nullptr);
    EXPECT_EQ(reloaded.find_forum("lobby")->default_character_id, "alpha");
    EXPECT_EQ(reloaded.find_forum("lobby")->display_name, "The Lobby");
    EXPECT_EQ(reloaded.find_forum("lobby")->description, "Where it starts");
}

TEST_F(SessionOpenTest, PersistsAChangedForumDefaultPersona) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");
    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);

    ASSERT_TRUE(opened.persist_default_persona);
    opened.persist_default_persona("reader");
    opened.controller->shutdown();
    opened.controller.reset();

    const WorkspaceDefinition reloaded = load_model();
    ASSERT_NE(reloaded.find_forum("lobby"), nullptr);
    EXPECT_EQ(reloaded.find_forum("lobby")->default_persona_id, "reader");
    const auto reloaded_sessions = make_repository(reloaded);
    OpenedSession later =
        open_session(reloaded, *reloaded_sessions, created.identity, notifier_);
    EXPECT_EQ(later.controller->view().default_persona_id, "reader");
    later.controller->shutdown();
}

TEST_F(SessionOpenTest, KeepsTheLoadedPersonaWhenTheConfigNamesAStranger) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");
    // Edited after startup to name a persona this workspace does not define.
    std::ofstream(fixture_.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_character = \"guide\"\n"
        << "default_persona = \"stranger\"\n";

    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);

    EXPECT_EQ(opened.controller->view().default_persona_id, guest_id);
    opened.controller->shutdown();
}

TEST_F(SessionOpenTest, KeepsTheLoadedDefaultWhenTheConfigNamesAStranger) {
    std::ofstream(fixture_.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_character = \"guide\"\n";
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");
    // Edited after startup to name a character this forum never loaded.
    std::ofstream(fixture_.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_character = \"stranger\"\n";

    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);

    EXPECT_EQ(opened.descriptor.forum_default_character_id, "guide");
    opened.controller->shutdown();
}

TEST_F(SessionOpenTest, ReplacesTheLegacyDefaultAgentKeyWhenItSaves) {
    const std::filesystem::path character = fixture_.root() / "characters" / "alpha";
    std::filesystem::create_directories(character);
    std::filesystem::create_directories(
        fixture_.root() / "forums" / "lobby" / "members" / "alpha");
    std::ofstream(character / "character.toml") << "display_name = \"Alpha\"\n";
    std::ofstream(character / "CHARACTER.md") << "Alpha instructions\n";
    std::ofstream(fixture_.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\ndefault_agent = \"guide\"\n";

    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");
    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);
    opened.persist_default_character("alpha");
    opened.controller->shutdown();

    // Loading rejects a config holding both spellings, so this also proves the
    // legacy key is gone.
    EXPECT_EQ(load_model().find_forum("lobby")->default_character_id, "alpha");
}

TEST_F(SessionOpenTest, OpensWelcomeThroughTheSamePathWithTheAssistantRoster) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);

    OpenedSession opened = open_session(model, *sessions, welcome(), notifier_);

    EXPECT_EQ(opened.descriptor.identity, welcome());
    EXPECT_EQ(opened.descriptor.forum_display_name, entrance_name);
    EXPECT_EQ(opened.descriptor.session_label, welcome_name);
    EXPECT_EQ(opened.descriptor.forum_default_character_id, assistant_id);
    EXPECT_EQ(opened.controller->view().default_persona_id, guest_id);
    ASSERT_EQ(opened.controller->view().characters.size(), 1U);
    EXPECT_EQ(opened.controller->view().characters.front().id, assistant_id);
    EXPECT_EQ(opened.controller->view().default_character_id, assistant_id);
    opened.controller->shutdown();
}

TEST_F(SessionOpenTest, GivesEveryControllerTheWorkspacePersonaRoster) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    OpenedSession stored = open_session(model, *sessions, created.identity, notifier_);
    OpenedSession entrance = open_session(model, *sessions, welcome(), notifier_);

    // A live session needs every workspace persona so /! can switch among
    // them, while unknown personas remain rejected.
    for (const OpenedSession& opened : {std::cref(stored), std::cref(entrance)}) {
        SessionController& controller = *opened.controller;
        EXPECT_EQ(
            controller.submit_prompt("outsider", "Hello").notice.value_or(""),
            "Unknown persona ID 'outsider'");
        EXPECT_EQ(controller.set_default_persona("reader").notice,
            "Current persona is now Reader");
        EXPECT_EQ(controller.view().default_persona_id, "reader");
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
    fixture_.write_provider("keyless",
        "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n"
        "api_key_env = \"CHA_TEST_UNSET_API_KEY\"\n");
    fixture_.write_character_defaults("provider = \"keyless\"\n");
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    EXPECT_THROW(
        (void)open_session(model, *sessions, created.identity, notifier_),
        std::runtime_error);

    // The failed open must not leave the session leased against the next one.
    EXPECT_NO_THROW((void)sessions->prepare(created.identity));
}

TEST_F(SessionOpenTest, ReResolvesCharacterDefinitionsWhenASessionOpens) {
    fixture_.write_style("serif-italic", "font = \"serif\"\nstyle = \"italic\"\n");
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    fixture_.write_character_config(
        "display_name = \"Renamed\"\nstyle = \"serif-italic\"\n");
    std::ofstream(fixture_.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"Renamed Lobby\"\n";

    {
        OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);
        // Discovery stays at startup; the session's characters re-resolve.
        EXPECT_EQ(opened.descriptor.forum_display_name, "The Lobby");
        EXPECT_EQ(opened.controller->view().characters.front().display_name, "Renamed");
        EXPECT_EQ(
            opened.controller->view().characters.front().appearance,
            (CharacterAppearance{
                CharacterFont::serif, CharacterSlant::italic,
                CharacterWeight::normal, CharacterScale::normal}));
        EXPECT_FALSE(opened.notice);
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

TEST_F(SessionOpenTest, FallsBackToStartupDefinitionsAndReportsWhenReloadFails) {
    const WorkspaceDefinition model = load_model();
    const auto sessions = make_repository(model);
    const StoredSession created = sessions->create("lobby", "Stored");

    std::ofstream(fixture_.root() / "characters" / "guide" / "CHARACTER.md")
        << "$$(missing.md)";

    OpenedSession opened = open_session(model, *sessions, created.identity, notifier_);
    EXPECT_EQ(opened.controller->view().characters.front().display_name, "Guide");
    ASSERT_TRUE(opened.notice);
    EXPECT_NE(opened.notice->find("could not be reloaded"), std::string::npos);
    opened.controller->shutdown();
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
    fixture_.write_character_config("display_name = \"Guide\"\n");
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
