#include "workspace/session_open.h"

#include "providers/providers.h"
#include "session/not_found_error.h"
#include "session/session_repository.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace cha {
namespace {

std::string config_content(
    const std::filesystem::path& database,
    std::string_view name) {
    sqlite3* handle = nullptr;
    if (sqlite3_open_v2(
            database.string().c_str(), &handle,
            SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to open test database");
    }
    sqlite3_stmt* statement = nullptr;
    const char sql[] = "SELECT content FROM config WHERE name = ?1";
    if (sqlite3_prepare_v2(handle, sql, -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(handle);
        throw std::runtime_error("Failed to prepare config query");
    }
    sqlite3_bind_text(
        statement, 1, name.data(), static_cast<int>(name.size()), SQLITE_STATIC);
    std::string result;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        const char* text =
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        if (text) result = text;
    }
    sqlite3_finalize(statement);
    sqlite3_close(handle);
    return result;
}

class SessionOpenTest : public ::testing::Test {
protected:
    SessionOpenTest() {
        database_ = test::import_test_database(fixture_.root());
        open_runtime();
    }

    void open_runtime() {
        sessions_.reset();
        store_.reset();
        store_ = WorkspaceConfigStore::open(database_);
        sessions_ = std::make_unique<SessionRepository>(
            store_->database_path(),
            store_->workspace_path(),
            store_->welcome_path(),
            TemporarySessionSeed{
                {std::string(entrance_id), std::string(welcome_id)},
                std::string(welcome_name)});
    }

    void reimport_fixture() {
        sessions_.reset();
        store_.reset();
        (void)import_workspace_configuration(fixture_.root(), database_);
        open_runtime();
    }

    OpenedSession open(const FullSessionId& identity) {
        return open_session(
            *sessions_, identity, providers_,
            std::make_shared<test::TestNotifier>(),
            *store_);
    }

    test::TestWorkspace fixture_;
    std::filesystem::path database_;
    Providers providers_;
    std::unique_ptr<WorkspaceConfigStore> store_;
    std::unique_ptr<SessionRepository> sessions_;
};

TEST_F(SessionOpenTest, OpensStoredSessionFromThePublishedWorkspace) {
    const StoredSession stored = sessions_->create("lobby", "Stored");
    OpenedSession opened = open(stored.identity);

    EXPECT_EQ(opened.label, "Stored");
    EXPECT_EQ(opened.controller->view().default_character_id, "guide");
    EXPECT_EQ(opened.controller->view().default_persona_id, workspace_guest_id);
    ASSERT_NE(getws()->find_forum("lobby"), nullptr);
}

TEST_F(SessionOpenTest, OpensWelcomeThroughTheSamePath) {
    OpenedSession opened = open(
        {std::string(entrance_id), std::string(welcome_id)});

    EXPECT_EQ(opened.label, welcome_name);
    EXPECT_EQ(
        opened.controller->view().default_persona_id,
        workspace_guest_id);
}

TEST_F(SessionOpenTest, DefaultWritesSurviveRestartAndExport) {
    sessions_.reset();
    store_.reset();
    fixture_.add_character("writer", "Writer");
    const auto member =
        fixture_.root() / "forums" / "lobby" / "members" / "writer";
    std::filesystem::create_directories(member);
    std::ofstream(member / "character.toml") << "# forum membership\n";
    reimport_fixture();
    StoredSession stored;
    {
        stored = sessions_->create("lobby", "Stored");
        OpenedSession opened = open(stored.identity);

        opened.persist_default_character("writer");
        const std::string after_character = config_content(
            store_->database_path(), "forums/lobby/config.toml");
        EXPECT_NE(after_character.find("writer"), std::string::npos);

        opened.persist_default_persona("reader");
    }

    const std::shared_ptr<const Workspace> workspace = getws();
    ASSERT_NE(workspace->find_forum("lobby"), nullptr);
    EXPECT_EQ(workspace->find_forum("lobby")->default_character_id, "writer");
    EXPECT_EQ(workspace->find_forum("lobby")->default_persona_id, "reader");

    const std::string forum = config_content(
        store_->database_path(), "forums/lobby/config.toml");
    EXPECT_NE(forum.find("writer"), std::string::npos);
    EXPECT_NE(forum.find("reader"), std::string::npos);

    sessions_.reset();
    store_.reset();
    open_runtime();
    ASSERT_NE(getws()->find_forum("lobby"), nullptr);
    EXPECT_EQ(getws()->find_forum("lobby")->default_character_id, "writer");
    EXPECT_EQ(getws()->find_forum("lobby")->default_persona_id, "reader");
    EXPECT_EQ(sessions_->prepare(stored.identity).label, "Stored");

    sessions_.reset();
    store_.reset();
    const std::filesystem::path exported = fixture_.root() / "exported";
    const WorkspaceConfigTransfer transfer =
        export_workspace_configuration(database_, exported);
    EXPECT_GT(transfer.file_count, 0U);
    std::ifstream exported_forum(exported / "forums" / "lobby" / "config.toml");
    const std::string exported_content{
        std::istreambuf_iterator<char>(exported_forum),
        std::istreambuf_iterator<char>()};
    EXPECT_NE(exported_content.find("writer"), std::string::npos);
    EXPECT_NE(exported_content.find("reader"), std::string::npos);
}

TEST_F(SessionOpenTest, SourceDirectoryEditsDoNotAffectOpening) {
    const StoredSession first = sessions_->create("lobby", "First");
    fixture_.write_character_config(
        "display_name = \"Renamed\"\nprovider = \"test\"\n");

    OpenedSession unchanged = open(first.identity);
    ASSERT_TRUE(unchanged.controller->character_information().notice);
    EXPECT_NE(
        unchanged.controller->character_information().notice->find("Guide"),
        std::string::npos);

    unchanged.controller.reset();
    sessions_.reset();
    store_.reset();
    reimport_fixture();
    const StoredSession second = sessions_->create("lobby", "Second");
    OpenedSession reloaded = open(second.identity);
    ASSERT_TRUE(reloaded.controller->character_information().notice);
    EXPECT_NE(
        reloaded.controller->character_information().notice->find("Renamed"),
        std::string::npos);
}

TEST_F(SessionOpenTest, ReportsMissingForumAndSession) {
    EXPECT_THROW(
        (void)open({"missing", "session"}),
        ForumNotFoundError);
    EXPECT_THROW(
        (void)open({"lobby", "missing"}),
        SessionNotFoundError);
}

} // namespace
} // namespace cha
