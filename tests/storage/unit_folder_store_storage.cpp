#include "storage/folder_store_storage.h"
#include "storage/event_storage.h"
#include "storage/session_database.h"
#include "storage/sqlite_storage.h"
#include "storage/workspace_session_database.h"
#include "support/test_workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

nlohmann::json text_doc(std::string_view text) {
    return nlohmann::json{
        {"type", "doc"},
        {"content", nlohmann::json::array({
            {{"type", "paragraph"},
             {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}},
        })},
    };
}

nlohmann::json empty_heading() {
    return nlohmann::json{
        {"type", "doc"},
        {"content", nlohmann::json::array({
            {{"type", "heading"}, {"attrs", {{"level", 1}}}, {"content", nlohmann::json::array()}},
        })},
    };
}

class FolderStoreStorageTest : public testing::Test {
protected:
    void SetUp() override {
        path_ = test::import_test_database(workspace_.root());
        database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    }

    test::TestWorkspace workspace_;
    std::filesystem::path path_;
    std::unique_ptr<Database> database_;
};

TEST_F(FolderStoreStorageTest, InstallsSchemaOnFreshAndExistingVaults) {
    EXPECT_TRUE(folder_store_tables_exist(*database_));
    database_->execute("DROP TABLE folder_store_files");
    database_->execute("DROP TABLE folder_stores");
    database_->execute("DROP TABLE session_folders");
    EXPECT_FALSE(folder_store_tables_exist(*database_));
    database_.reset();
    auto store = WorkspaceConfigStore::open(path_);
    EXPECT_TRUE(folder_store_tables_exist(
        *std::make_unique<Database>(path_, Database::Mode::read_only)));
    (void)store;
}

TEST(FolderStoreSchema, WelcomeAndTestJournalsIncludeFolderTables) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "welcome.sqlite3";
    ASSERT_TRUE(create_session_database(path, {.id = "welcome", .forum = "entrance", .label = "Welcome"}));
    Database database(path, Database::Mode::read_write);
    EXPECT_TRUE(folder_store_tables_exist(database));
    SessionJournal journal(path);
    journal.clear_history();
    EXPECT_TRUE(journal.folder_ids().empty());
}

TEST_F(FolderStoreStorageTest, RejectsRootNotesAndDuplicateOwnership) {
    EXPECT_THROW((void)folder_store_register(*database_, 0, FolderStoreService::openai, "api_key_1", 1), FolderStoreError);
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Note");
    EXPECT_FALSE(folder_store_require_folder(*database_, note.id));
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Research");
    storage::SqliteTransaction transaction(*database_);
    const auto first = folder_store_register(
        *database_, folder.id, FolderStoreService::openai, "api_key_1", 1);
    EXPECT_GT(first, 0);
    EXPECT_THROW(
        (void)folder_store_register(*database_, folder.id, FolderStoreService::google, "api_key_1", 1),
        std::exception);
    transaction.commit();
}

TEST_F(FolderStoreStorageTest, ReplacementGetsANewStoreIdAndReadyNeedsRemoteId) {
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Research");
    (void)note_create(*database_, folder.id, NoteItemKind::note, "Log");
    storage::SqliteTransaction transaction(*database_);
    const auto first = folder_store_register(
        *database_, folder.id, FolderStoreService::openai, "api_key_1", 1);
    folder_store_delete_record(*database_, first);
    const auto second = folder_store_register(
        *database_, folder.id, FolderStoreService::openai, "api_key_1", 1);
    EXPECT_NE(first, second);
    EXPECT_THROW(folder_store_mark_ready(*database_, second), FolderStoreError);
    folder_store_save_remote_id(*database_, second, "vs_1");
    folder_store_mark_ready(*database_, second);
    const auto ready = folder_store_get(*database_, second);
    ASSERT_TRUE(ready);
    EXPECT_EQ(ready->state, FolderStoreState::ready);
    EXPECT_TRUE(ready->uploaded_at);
    transaction.commit();
}

TEST_F(FolderStoreStorageTest, RestrictiveDeleteKeepsAStoreOwningFolder) {
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Research");
    storage::SqliteTransaction transaction(*database_);
    (void)folder_store_register(*database_, folder.id, FolderStoreService::openai, "api_key_1", 1);
    transaction.commit();
    EXPECT_THROW(database_->prepare("DELETE FROM note_items WHERE id=?1", folder.id).run(), std::runtime_error);
    EXPECT_EQ(sqlite3_extended_errcode(database_->handle()) & 0xff, SQLITE_CONSTRAINT);
    EXPECT_STREQ(sqlite3_errmsg(database_->handle()), "FOREIGN KEY constraint failed");
    EXPECT_THROW(note_delete(*database_, folder.id), NoteError);
    EXPECT_TRUE(folder_store_get_by_folder(*database_, folder.id));
}

TEST_F(FolderStoreStorageTest, CaptureExportsNestedFullTextAndSkipsEmptyFormatting) {
    const auto research = note_create(*database_, {}, NoteItemKind::folder, "Research");
    const auto experiments = note_create(*database_, research.id, NoteItemKind::folder, "Experiments");
    const auto log = note_create(*database_, experiments.id, NoteItemKind::note, "Experiment log");
    const auto duplicates = note_create(*database_, experiments.id, NoteItemKind::folder, "Copies");
    const auto copy = note_create(*database_, duplicates.id, NoteItemKind::note, "Experiment log");
    const auto empty = note_create(*database_, experiments.id, NoteItemKind::note, "Empty");
    const auto readonly = note_create(*database_, experiments.id, NoteItemKind::note, "Read only");
    note_save(*database_, log.id, text_doc("alpha body"));
    note_save(*database_, copy.id, text_doc("beta body"));
    note_save(*database_, empty.id, empty_heading());
    note_save(*database_, readonly.id, text_doc("gamma body"));
    (void)note_set_read_only(*database_, readonly.id, true);
    note_save_compressed(*database_, log.id, "compressed only");
    storage::SqliteTransaction transaction(*database_);
    const auto capture = folder_store_capture(*database_, research.id, FolderStoreService::openai);
    transaction.commit();
    ASSERT_EQ(capture.documents.size(), 3U);
    EXPECT_EQ(capture.documents[0].folder_path, "Research/Experiments");
    EXPECT_NE(capture.documents[0].markdown.find("alpha body"), std::string::npos);
    EXPECT_EQ(capture.documents[0].markdown.find("compressed only"), std::string::npos);
    EXPECT_EQ(capture.documents[0].filename, "Experiment-log-" + std::to_string(log.id) + ".md");
    EXPECT_EQ(capture.documents[1].filename, "Experiment-log-" + std::to_string(copy.id) + ".md");
    EXPECT_NE(capture.documents[0].markdown.find(note_item_ref_id(log.id)), std::string::npos);
    EXPECT_EQ(note_item_ref_id(11), "##00000B");
}

TEST_F(FolderStoreStorageTest, CaptureFailsWhenEveryNoteIsEmpty) {
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Empty");
    const auto note = note_create(*database_, folder.id, NoteItemKind::note, "Blank");
    note_save(*database_, note.id, empty_heading());
    storage::SqliteTransaction transaction(*database_);
    EXPECT_THROW((void)folder_store_capture(*database_, folder.id, FolderStoreService::openai), FolderStoreError);
}

TEST_F(FolderStoreStorageTest, CaptureFailsADamagedNonemptyDocument) {
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Damaged");
    const auto note = note_create(*database_, folder.id, NoteItemKind::note, "Broken");
    auto damaged = database_->prepare("UPDATE note_items SET content_json = ?1 WHERE id = ?2");
    damaged.bind_blob(1, "not-zlib");
    damaged.bind(2, note.id);
    damaged.run();
    storage::SqliteTransaction transaction(*database_);
    EXPECT_THROW((void)folder_store_capture(*database_, folder.id, FolderStoreService::openai), FolderStoreError);
}

TEST(FolderStoreJournal, AttachDetachClearAndNoticeRollback) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "journal.sqlite3";
    ASSERT_TRUE(create_session_database(path, {.id = "s", .forum = "f", .label = "S"}));
    Database database(path, Database::Mode::read_write);
    const auto folder = note_create(database, {}, NoteItemKind::folder, "Research");
    const auto note = note_create(database, folder.id, NoteItemKind::note, "Log");
    note_save(database, note.id, text_doc("body"));
    {
        storage::SqliteTransaction transaction(database);
        const auto store = folder_store_register(
            database, folder.id, FolderStoreService::openai, "api_key_1", 1);
        folder_store_save_remote_id(database, store, "vs_1");
        folder_store_mark_ready(database, store);
        transaction.commit();
    }
    SessionJournal journal(path);
    const auto added = journal.add_folder(folder.id, "Research", 1);
    EXPECT_NE(std::string(added.text).find("Added folder"), std::string::npos);
    EXPECT_EQ(journal.folder_ids(), std::vector<std::int64_t>({folder.id}));
    const auto again = journal.add_folder(folder.id, "Research", 2);
    EXPECT_NE(std::string(again.text).find("already attached"), std::string::npos);
    {
        storage::SqliteTransaction transaction(database);
        folder_store_mark_incomplete(database, folder_store_get_by_folder(database, folder.id)->id);
        transaction.commit();
    }
    const auto unavailable = journal.add_folder(folder.id, "Research", 3);
    EXPECT_NE(std::string(unavailable.text).find("already attached"), std::string::npos);
    const auto detached = journal.detach_folder(folder.id, "Research", 4);
    EXPECT_NE(std::string(detached.text).find("Removed folder"), std::string::npos);
    const auto repeated = journal.detach_folder(folder.id, "", 5);
    EXPECT_NE(std::string(repeated.text).find("not attached"), std::string::npos);
    EXPECT_NE(std::string(repeated.text).find(note_item_ref_id(folder.id)), std::string::npos);
    (void)journal.add_folder(folder.id, "Research", 6, [&](std::int64_t) { return true; });
    journal.clear_history();
    EXPECT_TRUE(journal.folder_ids().empty());
}

TEST(FolderStoreJournal, NoticeFailureRollsBackTheAssociation) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "journal.sqlite3";
    ASSERT_TRUE(create_session_database(path, {.id = "s", .forum = "f", .label = "S"}));
    SessionJournal journal(path);
    Database failure_db(path, Database::Mode::read_write);
    failure_db.execute("CREATE TRIGGER reject_notice BEFORE INSERT ON entries BEGIN SELECT RAISE(ABORT, 'notice failure'); END");
    EXPECT_THROW((void)journal.add_folder(1, "Research", 1, [](std::int64_t) { return true; }), std::exception);
    EXPECT_TRUE(journal.folder_ids().empty());
}

TEST(FolderStoreJournal, PersistentAttachAndDeleteSerializeOnSeparateConnections) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "race.sqlite3";
    create_empty_workspace_session_database(path);
    Database writer(path, Database::Mode::read_write);
    writer.execute("INSERT INTO forums (forum_id) VALUES ('f')");
    writer.execute(
        "INSERT INTO sessions (forum_key, session_id, label, updated_at, history_epoch, "
        "next_entry_id, next_request_id) VALUES (1, 's', 'S', 0, 1, 1, 1)");
    const auto folder = note_create(writer, {}, NoteItemKind::folder, "Research");
    const auto note = note_create(writer, folder.id, NoteItemKind::note, "Log");
    note_save(writer, note.id, text_doc("body"));
    {
        storage::SqliteTransaction transaction(writer);
        const auto store = folder_store_register(
            writer, folder.id, FolderStoreService::openai, "api_key_1", 1);
        folder_store_save_remote_id(writer, store, "vs_1");
        folder_store_mark_ready(writer, store);
        transaction.commit();
    }
    // Hold the deletion uncommitted while another journal connection attempts
    // BEGIN IMMEDIATE. It must wait, then recheck readiness after this commit.
    storage::SqliteTransaction deletion(writer);
    const auto record = folder_store_get_by_folder(writer, folder.id);
    folder_store_delete_record(writer, record->id);
    folder_store_delete_session_folders_in_subtree(writer, folder.id);
    note_delete_in_transaction(writer, folder.id);
    std::promise<void> attaching;
    auto attempted = attaching.get_future();
    auto attach = std::async(std::launch::async, [&] {
        SessionJournal journal(path, 1);
        attaching.set_value();
        try {
            (void)journal.add_folder(folder.id, "Research", 1);
            return std::string("unexpected attachment");
        } catch (const std::invalid_argument& error) {
            return std::string(error.what());
        }
    });
    EXPECT_EQ(attempted.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(attach.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
    deletion.commit();
    EXPECT_EQ(attach.get(), "That folder does not have a ready store.");
    Database check(path, Database::Mode::read_only);
    auto dangling = check.prepare(
        "SELECT COUNT(*) FROM session_folders");
    ASSERT_TRUE(dangling.step());
    EXPECT_EQ(dangling.integer(0), 0);
}

TEST(FolderStoreRefId, EncodesCompleteNumericIds) {
    EXPECT_EQ(note_item_ref_id(1), "##000001");
    EXPECT_EQ(note_item_ref_id(11), "##00000B");
    EXPECT_EQ(note_item_ref_id(62), "##000010");
}

TEST_F(FolderStoreStorageTest, MetadataIsEscapedAndFilenameKeepsUtf8AndCompleteId) {
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "[Research]*");
    const auto note = note_create(*database_, folder.id, NoteItemKind::note, "# Fact [one]");
    note_save(*database_, note.id, text_doc("*saved body*"));
    const auto capture = folder_store_capture(*database_, folder.id, FolderStoreService::google);
    ASSERT_EQ(capture.documents.size(), 1U);
    EXPECT_NE(capture.documents[0].markdown.find("# Note: \\# Fact \\[one\\]"), std::string::npos);
    EXPECT_NE(capture.documents[0].markdown.find("Folder: \\[Research\\]\\*"), std::string::npos);
    EXPECT_TRUE(capture.documents[0].markdown.ends_with(note_document_markdown(text_doc("*saved body*"))));
    EXPECT_EQ(note_item_ref_id(56800235584LL), "##1000000");
    const auto filename = folder_store_filename(std::string(100, 'a') + "é/\\\n", note_id_limit);
    EXPECT_LE(filename.size(), 101U);
    EXPECT_TRUE(filename.ends_with("-9007199254740991.md"));
    EXPECT_EQ(filename.find('/'), std::string::npos);
    EXPECT_EQ(filename.find('\\'), std::string::npos);
}

TEST_F(FolderStoreStorageTest, EmptyFormattingAndImageAltFollowTheExporter) {
    nlohmann::json document = {{"type", "doc"}, {"content", nlohmann::json::array({
        {{"type", "codeBlock"}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", " \t\n"}}})}},
        {{"type", "heading"}}, {{"type", "hardBreak"}}, {{"type", "image"}, {"attrs", {{"alt", ""}}}}
    })}};
    EXPECT_FALSE(folder_store_document_nonempty(document));
    document["content"].push_back({{"type", "image"}, {"attrs", {{"alt", "image description"}}}});
    EXPECT_TRUE(folder_store_document_nonempty(document));
    EXPECT_NE(note_document_markdown(document).find("image description"), std::string::npos);
}

TEST(FolderStoreLimits, ChecksFinalByteBoundariesWithoutTruncation) {
    for (const auto service : {FolderStoreService::openai, FolderStoreService::google}) {
        const auto limit = folder_store_document_byte_limit(service);
        EXPECT_NO_THROW(folder_store_validate_document_size(service, limit, "Log", 11));
        try {
            folder_store_validate_document_size(service, limit + 1, "Log", 11);
            FAIL() << "Oversize document was accepted";
        } catch (const FolderStoreError& error) {
            EXPECT_NE(std::string(error.what()).find("Log"), std::string::npos);
            EXPECT_NE(std::string(error.what()).find("11"), std::string::npos);
        }
    }
}

TEST_F(FolderStoreStorageTest, StaleAttemptCannotChangeReplacementAndUncertaintyPreventsReady) {
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Research");
    const auto first = folder_store_register(*database_, folder.id, FolderStoreService::openai, "api_key_1", 1);
    folder_store_delete_record(*database_, first);
    const auto next = folder_store_register(*database_, folder.id, FolderStoreService::google, "api_key_2", 1);
    EXPECT_THROW(folder_store_save_remote_id(*database_, first, "old"), FolderStoreError);
    folder_store_save_remote_id(*database_, next, "fileSearchStores/new");
    folder_store_set_uncertainty(*database_, next, true);
    EXPECT_THROW(folder_store_mark_ready(*database_, next), FolderStoreError);
    EXPECT_EQ(folder_store_get(*database_, next)->remote_store_id, "fileSearchStores/new");
}

TEST_F(FolderStoreStorageTest, ReadyEventAndNotFoundWarningRollBackWithTheirState) {
    database_.reset();
    auto store = WorkspaceConfigStore::open(path_);
    const auto folder = store->create_note_item({}, NoteItemKind::folder, "Research");
    const auto id = store->register_folder_store(folder.id, FolderStoreService::openai, "api_key_1", 1);
    store->save_folder_store_remote_id(id, "vs_1");
    Database failure_db(path_, Database::Mode::read_write);
    failure_db.execute("CREATE TABLE commit_failure(id INTEGER REFERENCES note_items(id) DEFERRABLE INITIALLY DEFERRED)");
    const auto reject_commit = [&] {
        failure_db.execute("CREATE TRIGGER reject_commit AFTER INSERT ON event_history "
            "BEGIN INSERT INTO commit_failure VALUES (-1); END");
    };
    reject_commit();
    EXPECT_THROW(store->mark_folder_store_ready(id, "Uploaded"), std::exception);
    EXPECT_EQ(store->get_folder_store(id)->state, FolderStoreState::incomplete);
    EXPECT_TRUE(store->list_events().empty());
    failure_db.execute("DROP TRIGGER reject_commit");
    store->add_folder_store_file_id(id, "file_1");
    failure_db.execute("CREATE TRIGGER reject_warning BEFORE INSERT ON event_history BEGIN SELECT RAISE(ABORT, 'warning failure'); END");
    EXPECT_THROW(store->remove_folder_store_file_id(id, "file_1", "Not found"), std::exception);
    EXPECT_EQ(store->get_folder_store(id)->file_ids, std::vector<std::string>({"file_1"}));
    EXPECT_TRUE(store->list_events().empty());
    failure_db.execute("DROP TRIGGER reject_warning");
    reject_commit();
    EXPECT_THROW(store->delete_folder_store_record(id, "Removed"), std::exception);
    EXPECT_TRUE(store->get_folder_store(id));
    EXPECT_TRUE(store->list_events().empty());
}

TEST(FolderStoreJournal, DetachNoticeFailureKeepsTheAssociation) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "journal.sqlite3";
    ASSERT_TRUE(create_session_database(path, {.id = "s", .forum = "f", .label = "S"}));
    SessionJournal journal(path);
    (void)journal.add_folder(11, "Research", 1, [](auto) { return true; });
    Database database(path, Database::Mode::read_write);
    database.execute("CREATE TRIGGER reject_notice BEFORE INSERT ON entries BEGIN SELECT RAISE(ABORT, 'notice failure'); END");
    EXPECT_THROW((void)journal.detach_folder(11, "Research", 2), std::exception);
    EXPECT_EQ(journal.folder_ids(), std::vector<std::int64_t>({11}));
}

TEST(FolderStoreJournal, AssociationsAreSessionScopedAndCascadeWithSessionDeletion) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "journal.sqlite3";
    ASSERT_TRUE(create_session_database(path, {.id = "s", .forum = "f", .label = "S"}));
    Database database(path, Database::Mode::read_write);
    database.execute("INSERT INTO sessions (forum_key, session_id, label, updated_at, history_epoch, next_entry_id, next_request_id) VALUES (1,'other','Other',0,1,1,1)");
    SessionJournal first(path, 1), second(path, 2);
    (void)first.add_folder(11, "First", 1, [](auto) { return true; });
    (void)second.add_folder(22, "Second", 1, [](auto) { return true; });
    first.clear_history();
    EXPECT_TRUE(first.folder_ids().empty());
    EXPECT_EQ(second.folder_ids(), std::vector<std::int64_t>({22}));
    database.execute("DELETE FROM sessions WHERE session_key=2");
    auto row = database.prepare("SELECT COUNT(*) FROM session_folders");
    ASSERT_TRUE(row.step());
    EXPECT_EQ(row.integer(0), 0);
}

} // namespace
} // namespace cha
