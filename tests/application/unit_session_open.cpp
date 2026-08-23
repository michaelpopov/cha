#include "workspace/session_open.h"

#include "providers/providers.h"
#include "session/not_found_error.h"
#include "session/session_repository.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

namespace cha {
namespace {

class SessionOpenTest : public ::testing::Test {
protected:
    SessionOpenTest() {
        loadws(fixture_.root());
        sessions_ = std::make_unique<SessionRepository>(
            fixture_.root(),
            TemporarySessionSeed{
                {std::string(entrance_id), std::string(welcome_id)},
                std::string(welcome_name)});
    }

    OpenedSession open(const SessionIdentity& identity) {
        return open_session(
            *sessions_, identity, providers_,
            std::make_shared<test::TestNotifier>());
    }

    test::TestWorkspace fixture_;
    Providers providers_;
    std::unique_ptr<SessionRepository> sessions_;
};

TEST_F(SessionOpenTest, OpensStoredSessionFromThePublishedWorkspace) {
    const StoredSession stored = sessions_->create("lobby", "Stored");
    OpenedSession opened = open(stored.identity);

    EXPECT_EQ(opened.descriptor.identity, stored.identity);
    EXPECT_EQ(opened.descriptor.forum_display_name, "The Lobby");
    EXPECT_EQ(opened.descriptor.session_label, "Stored");
    EXPECT_EQ(opened.descriptor.forum_default_character_id, "guide");
    EXPECT_EQ(opened.controller->view().default_character_id, "guide");
    EXPECT_EQ(opened.controller->view().default_persona_id, workspace_guest_id);
    ASSERT_NE(getws()->find_forum("lobby"), nullptr);
}

TEST_F(SessionOpenTest, OpensWelcomeThroughTheSamePath) {
    OpenedSession opened = open(
        {std::string(entrance_id), std::string(welcome_id)});

    EXPECT_EQ(opened.descriptor.forum_display_name, "Entrance");
    EXPECT_EQ(opened.descriptor.session_label, welcome_name);
    EXPECT_EQ(
        opened.descriptor.forum_default_character_id,
        workspace_assistant_id);
    EXPECT_EQ(
        opened.controller->view().default_persona_id,
        workspace_guest_id);
}

TEST_F(SessionOpenTest, DefaultWritesReloadThePublishedWorkspace) {
    fixture_.add_character("writer", "Writer");
    std::filesystem::create_directories(
        fixture_.root() / "forums" / "lobby" / "members" / "writer");
    loadws(fixture_.root());
    const StoredSession stored = sessions_->create("lobby", "Stored");
    OpenedSession opened = open(stored.identity);

    opened.persist_default_character("writer");
    opened.persist_default_persona("reader");

    const std::shared_ptr<const Workspace> workspace = getws();
    ASSERT_NE(workspace->find_forum("lobby"), nullptr);
    EXPECT_EQ(workspace->find_forum("lobby")->default_character_id, "writer");
    EXPECT_EQ(workspace->find_forum("lobby")->default_persona_id, "reader");
}

TEST_F(SessionOpenTest, DiskEditsDoNotAffectOpeningUntilLoadws) {
    const StoredSession first = sessions_->create("lobby", "First");
    fixture_.write_character_config(
        "display_name = \"Renamed\"\nprovider = \"test\"\n");

    OpenedSession unchanged = open(first.identity);
    ASSERT_TRUE(unchanged.controller->character_information().notice);
    EXPECT_NE(
        unchanged.controller->character_information().notice->find("Guide"),
        std::string::npos);

    unchanged.controller.reset();
    loadws(fixture_.root());
    const StoredSession second = sessions_->create("lobby", "Second");
    OpenedSession reloaded = open(second.identity);
    ASSERT_TRUE(reloaded.controller->character_information().notice);
    EXPECT_NE(
        reloaded.controller->character_information().notice->find("Renamed"),
        std::string::npos);
}

TEST_F(SessionOpenTest, ReportsMissingForumSessionAndLeaseContention) {
    EXPECT_THROW(
        (void)open({"missing", "session"}),
        ForumNotFoundError);
    EXPECT_THROW(
        (void)open({"lobby", "missing"}),
        SessionNotFoundError);

    const StoredSession stored = sessions_->create("lobby", "Stored");
    OpenedSession first = open(stored.identity);
    EXPECT_THROW((void)open(stored.identity), SessionBusyError);
}

} // namespace
} // namespace cha
