#include "storage/folder_store_storage.h"
#include "storage/sqlite_storage.h"
#include "support/test_workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
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

bool table_exists(Database& database, std::string_view name) {
    auto row = database.prepare("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1");
    row.bind(1, name);
    return row.step();
}

TEST_F(FolderStoreStorageTest, OpeningAnOlderVaultDropsLegacyFolderTables) {
    // The schema of earlier CHA versions, with a ready store and a file.
    database_->execute(R"sql(
CREATE TABLE folder_stores (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    folder_id INTEGER NOT NULL UNIQUE REFERENCES note_items(id) ON DELETE RESTRICT,
    service TEXT NOT NULL CHECK (service IN ('openai', 'google')),
    api_key_id TEXT NOT NULL,
    remote_store_id TEXT,
    state TEXT NOT NULL CHECK (state IN ('incomplete', 'ready')),
    note_count INTEGER NOT NULL CHECK (note_count > 0),
    uploaded_at INTEGER,
    remote_outcome_unknown INTEGER NOT NULL DEFAULT 0 CHECK (remote_outcome_unknown IN (0, 1)),
    CHECK (state <> 'ready' OR (remote_store_id IS NOT NULL AND uploaded_at IS NOT NULL
        AND remote_outcome_unknown = 0))
) STRICT;
CREATE TABLE folder_store_files (
    store_id INTEGER NOT NULL REFERENCES folder_stores(id) ON DELETE CASCADE,
    file_id TEXT NOT NULL,
    PRIMARY KEY (store_id, file_id)
) STRICT;
CREATE TABLE session_folders (
    session_key INTEGER NOT NULL REFERENCES sessions(session_key) ON DELETE CASCADE,
    folder_id INTEGER NOT NULL CHECK (folder_id > 0),
    PRIMARY KEY (session_key, folder_id)
) STRICT;
)sql");
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Research");
    (void)note_create(*database_, folder.id, NoteItemKind::note, "Log");
    database_->prepare("INSERT INTO folder_stores (folder_id, service, api_key_id, remote_store_id, state, "
        "note_count, uploaded_at) VALUES (?1, 'openai', 'api_key_1', 'vs_1', 'ready', 1, 1)", folder.id).run();
    database_->prepare("INSERT INTO folder_store_files VALUES (?1, 'file_1')", database_->last_insert_rowid()).run();
    database_.reset();
    auto store = WorkspaceConfigStore::open(path_);
    Database check(path_, Database::Mode::read_only);
    EXPECT_FALSE(table_exists(check, "folder_stores"));
    EXPECT_FALSE(table_exists(check, "folder_store_files"));
    EXPECT_FALSE(table_exists(check, "session_folders"));
    store->delete_note_item(folder.id);
    EXPECT_FALSE(store->get_note_item(folder.id));
}

TEST_F(FolderStoreStorageTest, RequireFolderRejectsNotes) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Note");
    EXPECT_FALSE(folder_store_require_folder(*database_, note.id));
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Research");
    EXPECT_TRUE(folder_store_require_folder(*database_, folder.id));
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

} // namespace
} // namespace cha
