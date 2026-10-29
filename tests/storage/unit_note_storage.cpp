#include "storage/note_storage.h"
#include "storage/folder_store_storage.h"
#include "storage/session_label.h"
#include "storage/session_database.h"
#include "storage/sqlite_storage.h"
#include "storage/workspace_session_database.h"
#include "support/test_workspace.h"
#include "util/logging.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;

template<typename Fn>
void expect_note_error(NoteFailure failure, std::string_view message, Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
        FAIL() << "expected note error: " << message;
    } catch (const NoteError& error) {
        EXPECT_EQ(static_cast<int>(error.failure), static_cast<int>(failure));
        EXPECT_EQ(std::string(error.what()), std::string(message));
    }
}

void expect_invalid_argument(std::string_view message, auto&& fn) {
    try {
        fn();
        FAIL() << "expected invalid argument: " << message;
    } catch (const std::invalid_argument& error) {
        EXPECT_EQ(std::string(error.what()), std::string(message));
    }
}

std::string repeated_code_point(std::size_t count) {
    std::string text;
    text.reserve(count * 2);
    for (std::size_t index = 0; index < count; ++index) {
        text.push_back(static_cast<char>(0xc3));
        text.push_back(static_cast<char>(0xa9));
    }
    return text;
}

int item_count(Database& database) {
    Statement statement = database.prepare("SELECT COUNT(*) FROM note_items");
    EXPECT_TRUE(statement.step());
    return static_cast<int>(statement.integer(0));
}

std::string compressed_content(std::string_view text) {
    uLongf size = compressBound(static_cast<uLong>(text.size()));
    std::string bytes(size, '\0');
    EXPECT_EQ(compress(reinterpret_cast<Bytef*>(bytes.data()), &size,
        reinterpret_cast<const Bytef*>(text.data()), static_cast<uLong>(text.size())), Z_OK);
    bytes.resize(size);
    return bytes;
}

std::string raw_content(Database& database, std::int64_t id) {
    Statement statement = database.prepare(
        "SELECT content_json FROM note_items WHERE id = ?1");
    statement.bind(1, id);
    EXPECT_TRUE(statement.step());
    EXPECT_FALSE(statement.is_null(0));
    const auto bytes = statement.blob(0);
    uLongf size = note_document_byte_limit;
    std::string text(size, '\0');
    EXPECT_EQ(uncompress(reinterpret_cast<Bytef*>(text.data()), &size,
        reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uLong>(bytes.size())), Z_OK);
    text.resize(size);
    return text;
}

std::int64_t note_sequence(Database& database) {
    Statement statement = database.prepare(
        "SELECT seq FROM sqlite_sequence WHERE name = 'note_items'");
    EXPECT_TRUE(statement.step());
    return statement.integer(0);
}

bool has_schema_object(
    Database& database,
    std::string_view type,
    std::string_view name) {
    Statement statement = database.prepare(
        "SELECT 1 FROM sqlite_master WHERE type = ?1 AND name = ?2");
    statement.bind(1, type);
    statement.bind(2, name);
    return statement.step();
}

nlohmann::json load_byte_fixture() {
    const auto path = std::filesystem::path(CHA_SOURCE_DIRECTORY)
        / "tests/fixtures/notes/document-byte-limits.json";
    std::ifstream input(path);
    EXPECT_TRUE(input) << path;
    return nlohmann::json::parse(input);
}

nlohmann::json padded_document(const nlohmann::json& base, std::size_t count) {
    nlohmann::json document = base;
    document["content"][0]["content"][0]["text"] = std::string(count, 'a');
    return document;
}

class NoteStorageTest : public testing::Test {
protected:
    void SetUp() override {
        path_ = workspace_.root() / "notes.sqlite3";
        create_empty_workspace_session_database(path_);
        database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    }

    test::TestWorkspace workspace_;
    std::filesystem::path path_;
    std::unique_ptr<Database> database_;
};


nlohmann::json annotation_fixture();

nlohmann::json summary_document(std::string text = "Summary body") {
    return {{"type", "doc"}, {"content", {{{"type", "paragraph"},
        {"content", {{{"type", "text"}, {"text", std::move(text)}}}}}}}};
}

void summary_source(Database& database, std::string_view id = "source", std::string_view label = "Topic") {
    create_event_history_schema(database);
    database.execute("INSERT OR IGNORE INTO forums(forum_key,forum_id) VALUES(1,'a')");
    database.prepare("INSERT INTO sessions(forum_key,session_id,label,updated_at,history_epoch,next_entry_id,next_request_id) "
        "VALUES(1,?,?,0,1,1,1)", id, label).run();
    storage::SqliteTransaction transaction(database);
    note_sync_forums(database, {{"a", "Forum A"}});
    transaction.commit();
}

TEST_F(NoteStorageTest, SummaryReplacementKeepsIdentityAndClearsAudioCommentsAndCompression) {
    summary_source(*database_);
    const FullSessionId source{"a", "source"};
    const auto first = note_commit_summary(*database_, source, note_summary_target(*database_, source), summary_document());
    EXPECT_TRUE(first.is_summary);
    EXPECT_TRUE(first.summary_linked);
    EXPECT_TRUE(first.read_only);
    EXPECT_EQ(first.name, "Topic");
    auto wire = nlohmann::json(first);
    EXPECT_EQ(wire["is_summary"], true);
    EXPECT_EQ(wire["summary_linked"], true);
    EXPECT_THROW((void)note_rename(*database_, first.id, "Topic"), NoteError);
    (void)note_set_read_only(*database_, first.id, false);
    (void)note_save(*database_, first.id, summary_document("User edits"));
    note_save_compressed(*database_, first.id, "Old compressed text");
    note_save_scroll_position(*database_, first.id, 100);
    database_->prepare("INSERT INTO note_audio VALUES(?,0,'audio/mpeg',X'0102')", first.id).run();
    (void)note_set_read_only(*database_, first.id, true);
    const auto attempt = note_audio_accept(*database_, first.id, 1, "http://localhost", "key", [](const auto&) {});
    ASSERT_TRUE(attempt);
    const auto saved = note_commit_summary(*database_, source, note_summary_target(*database_, source), summary_document("Replacement"));
    EXPECT_EQ(saved.id, first.id);
    EXPECT_EQ(saved.name, first.name);
    EXPECT_TRUE(saved.read_only);
    EXPECT_FALSE(saved.has_compressed);
    EXPECT_EQ(note_get(*database_, saved.id).scroll_top, 0);
    EXPECT_EQ(note_get(*database_, saved.id).document, summary_document("Replacement"));
    EXPECT_EQ(note_audio_info(*database_, saved.id)["count"], 0);
    EXPECT_FALSE(note_audio_attempt_for_note(*database_, saved.id));
    EXPECT_FALSE(note_audio_finish(*database_, attempt->id, {}));
    EXPECT_EQ(note_list(*database_).size(), 3U); // No empty ANNOTATIONS.
}

TEST_F(NoteStorageTest, SummaryReplacementRebuildsOnlyItsAnnotationsAndPreservesChatCopies) {
    summary_source(*database_, "first", "First source");
    summary_source(*database_, "second", "Second source");
    const auto first = note_commit_summary(*database_, {"a", "first"}, note_summary_target(*database_, {"a", "first"}), summary_document());
    const auto second = note_commit_summary(*database_, {"a", "second"}, note_summary_target(*database_, {"a", "second"}), summary_document());
    auto other_document = annotation_fixture()["document"];
    other_document["content"][2]["content"][1]["text"] = "Other note comment";
    (void)note_set_read_only(*database_, first.id, false);
    (void)note_set_read_only(*database_, second.id, false);
    EXPECT_TRUE(note_save(*database_, first.id, annotation_fixture()["document"]));
    EXPECT_FALSE(note_save(*database_, second.id, other_document));
    const auto items = note_list(*database_);
    const auto annotation = std::find_if(items.begin(), items.end(), [&](const auto& item) {
        return item.parent_id == first.parent_id && item.kind == NoteItemKind::annotation;
    });
    ASSERT_NE(annotation, items.end());
    SessionJournal journal(path_, 1);
    const auto copy = note_get_text(*database_, first.id);
    (void)journal.add_note(copy, 1);
    journal.rename("Renamed source");
    EXPECT_NE(note_get_text(*database_, annotation->id).text.find("Renamed source"), std::string::npos);
    (void)note_commit_summary(*database_, {"a", "first"}, note_summary_target(*database_, {"a", "first"}), summary_document("Regenerated"));
    const auto annotated = note_get_text(*database_, annotation->id).text;
    EXPECT_EQ(annotated.find("Renamed source"), std::string::npos);
    EXPECT_NE(annotated.find("Other note comment"), std::string::npos);
    note_delete(*database_, first.id);
    auto saved_copy = database_->prepare("SELECT text FROM session_notes WHERE note_id=?", first.id);
    ASSERT_TRUE(saved_copy.step());
    EXPECT_EQ(saved_copy.text(0), copy.text);
    EXPECT_NE(note_get_text(*database_, annotation->id).text.find("Other note comment"), std::string::npos);
}

TEST_F(NoteStorageTest, SummaryCommitRollsBackDocumentAudioAndEventOnFailure) {
    summary_source(*database_);
    const FullSessionId source{"a", "source"};
    const auto first = note_commit_summary(*database_, source, note_summary_target(*database_, source), summary_document());
    database_->prepare("INSERT INTO note_audio VALUES(?,0,'audio/mpeg',X'0102')", first.id).run();
    const auto attempt = note_audio_accept(*database_, first.id, 1, "http://localhost", "key", [](const auto&) {});
    ASSERT_TRUE(attempt);
    database_->prepare("UPDATE note_items SET read_only=0 WHERE id=?", first.id).run();
    const auto events = event_list(*database_).size();
    database_->execute("CREATE TRIGGER fail_summary BEFORE INSERT ON event_history "
        "WHEN NEW.severity='success' BEGIN SELECT RAISE(ABORT,'injected summary failure'); END");
    EXPECT_THROW((void)note_commit_summary(*database_, source, note_summary_target(*database_, source), summary_document("New")), NoteError);
    EXPECT_EQ(note_get(*database_, first.id).document, summary_document());
    EXPECT_FALSE(note_find_item(*database_, first.id)->read_only);
    EXPECT_TRUE(note_audio_attempt_for_note(*database_, first.id));
    EXPECT_EQ(note_audio_info(*database_, first.id)["count"], 1);
    EXPECT_EQ(event_list(*database_).size(), events);
}

TEST_F(NoteStorageTest, SummaryDeletionDetachesBeforeSessionKeyReuseAndNeverRecreatesDeletedTarget) {
    summary_source(*database_);
    const FullSessionId source{"a", "source"};
    const auto saved = note_commit_summary(*database_, source, note_summary_target(*database_, source), summary_document());
    database_->execute("DELETE FROM sessions");
    const auto detached = *note_find_item(*database_, saved.id);
    EXPECT_TRUE(detached.is_summary);
    EXPECT_FALSE(detached.summary_linked);
    EXPECT_EQ(note_rename(*database_, saved.id, "Retained").name, "Retained");
    summary_source(*database_, "new", "Topic"); // Reuses integer session key 1.
    EXPECT_FALSE(note_summary_target(*database_, {"a", "new"}).target_note_id);
    EXPECT_THROW((void)note_commit_summary(*database_, source, { *saved.parent_id, {} }, summary_document()), NoteError);
    const auto second = note_commit_summary(*database_, {"a", "new"}, note_summary_target(*database_, {"a", "new"}), summary_document());
    const auto target = note_summary_target(*database_, {"a", "new"});
    note_delete(*database_, second.id);
    EXPECT_THROW((void)note_commit_summary(*database_, {"a", "new"}, target, summary_document()), NoteError);
    const auto third = note_commit_summary(*database_, {"a", "new"}, note_summary_target(*database_, {"a", "new"}), summary_document());
    EXPECT_GT(third.id, second.id);
    EXPECT_FALSE(database_->prepare("PRAGMA foreign_key_check").step());
}

TEST_F(NoteStorageTest, SummaryNamesKeepDigitsCaseAndUnicodeAndPreserveSuffixes) {
    summary_source(*database_, "first", "Topic 1");
    summary_source(*database_, "second", "Topic 1");
    const auto first = note_commit_summary(*database_, {"a", "first"}, note_summary_target(*database_, {"a", "first"}), summary_document());
    const auto second = note_commit_summary(*database_, {"a", "second"}, note_summary_target(*database_, {"a", "second"}), summary_document());
    EXPECT_EQ(first.name, "Topic 1");
    EXPECT_EQ(second.name, "Topic 1 1");
    note_delete(*database_, first.id);
    const auto again = note_commit_summary(*database_, {"a", "second"}, note_summary_target(*database_, {"a", "second"}), summary_document());
    EXPECT_EQ(again.name, "Topic 1 1");
    summary_source(*database_, "reserved", "ANNOTATIONS");
    EXPECT_EQ(note_commit_summary(*database_, {"a", "reserved"}, note_summary_target(*database_, {"a", "reserved"}), summary_document()).name, "ANNOTATIONS 1");
    summary_source(*database_, "case", "topic 1");
    EXPECT_EQ(note_commit_summary(*database_, {"a", "case"}, note_summary_target(*database_, {"a", "case"}), summary_document()).name, "topic 1");
    const auto long_label = repeated_code_point(200);
    summary_source(*database_, "long1", long_label);
    summary_source(*database_, "long2", long_label);
    EXPECT_EQ(note_commit_summary(*database_, {"a", "long1"}, note_summary_target(*database_, {"a", "long1"}), summary_document()).name, long_label);
    const auto suffixed = note_commit_summary(*database_, {"a", "long2"}, note_summary_target(*database_, {"a", "long2"}), summary_document()).name;
    EXPECT_EQ(suffixed, repeated_code_point(198) + " 1");
    EXPECT_NO_THROW(validate_display_label(suffixed, "Name"));
}

TEST_F(NoteStorageTest, SummaryRenameIsTransactionalAndNoOpKeepsCollisionSuffix) {
    summary_source(*database_, "first", "Topic");
    summary_source(*database_, "second", "Topic");
    const auto first = note_commit_summary(*database_, {"a", "first"}, note_summary_target(*database_, {"a", "first"}), summary_document());
    const auto second = note_commit_summary(*database_, {"a", "second"}, note_summary_target(*database_, {"a", "second"}), summary_document());
    note_delete(*database_, first.id);
    SessionJournal journal(path_, 2);
    journal.rename("Topic");
    EXPECT_EQ(note_find_item(*database_, second.id)->name, "Topic 1");
    journal.rename("New title");
    EXPECT_EQ(note_find_item(*database_, second.id)->name, "New title");
    database_->execute("CREATE TRIGGER fail_summary_rename BEFORE UPDATE OF name ON note_items "
        "BEGIN SELECT RAISE(ABORT,'injected failure'); END");
    EXPECT_THROW(journal.rename("Failed title"), std::exception);
    auto label = database_->prepare("SELECT label FROM sessions WHERE session_key=2");
    ASSERT_TRUE(label.step());
    EXPECT_EQ(label.text(0), "New title");
    EXPECT_EQ(note_find_item(*database_, second.id)->name, "New title");
}

TEST_F(NoteStorageTest, SummaryUpgradeOfBlobNotesIsAdditiveIdempotentAndUsable) {
    summary_source(*database_);
    const auto legacy = note_create(*database_, {}, NoteItemKind::note, "Existing note");
    (void)note_save(*database_, legacy.id, annotation_fixture()["document"]);
    database_->prepare("INSERT INTO note_audio VALUES(?,0,'audio/mpeg',X'0102')", legacy.id).run();
    database_->prepare("INSERT INTO session_notes VALUES(1,?,'Existing note','Saved chat copy')", legacy.id).run();
    const auto blob = raw_content(*database_, legacy.id);
    database_->execute("DROP TABLE note_summaries");
    for (int reopen = 0; reopen < 2; ++reopen) {
        database_.reset();
        database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
        storage::SqliteTransaction tx(*database_);
        create_note_items_schema(*database_);
        tx.commit();
        EXPECT_EQ(raw_content(*database_, legacy.id), blob);
        EXPECT_EQ(note_audio_info(*database_, legacy.id)["count"], 1);
        EXPECT_FALSE(note_find_item(*database_, legacy.id)->is_summary);
        EXPECT_FALSE(database_->prepare("PRAGMA foreign_key_check").step());
    }
    const auto summary = note_commit_summary(*database_, {"a", "source"}, note_summary_target(*database_, {"a", "source"}), summary_document());
    EXPECT_TRUE(summary.summary_linked);
    auto copy = database_->prepare("SELECT text FROM session_notes WHERE note_id=?", legacy.id);
    ASSERT_TRUE(copy.step());
    EXPECT_EQ(copy.text(0), "Saved chat copy");
}

TEST_F(NoteStorageTest, SummaryValidationAndForumRemovalAndManagedStoreAdmission) {
    summary_source(*database_);
    const FullSessionId source{"a", "source"};
    const auto target = note_summary_target(*database_, source);
    for (const auto& invalid : {nlohmann::json::object(), nlohmann::json{{"type", "doc"}, {"content", "bad"}},
        nlohmann::json{{"type", "doc"}, {"content", {1}}}, summary_document(" \n\t ")}) {
        EXPECT_THROW((void)note_commit_summary(*database_, source, target, invalid), NoteError);
    }
    EXPECT_THROW((void)note_create(*database_, target.parent_id, NoteItemKind::note, "Ordinary"), NoteError);
    EXPECT_THROW((void)folder_store_register(*database_, target.parent_id, FolderStoreService::openai, "key", 1), FolderStoreError);
    const auto saved = note_commit_summary(*database_, source, target, summary_document());
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {});
        tx.commit();
    }
    EXPECT_FALSE(note_find_item(*database_, saved.id));
    EXPECT_FALSE(database_->prepare("SELECT 1 FROM note_summaries").step());
    EXPECT_FALSE(database_->prepare("PRAGMA foreign_key_check").step());
}

TEST_F(NoteStorageTest, ForumSchemaIsAdditiveAndMarkersAreUnique) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Legacy");
    const auto blob = raw_content(*database_, note.id);
    database_->execute("DROP INDEX note_items_forum_id");
    database_->execute("ALTER TABLE note_items DROP COLUMN forum_id");
    const auto table_sql = [&] {
        auto q = database_->prepare("SELECT sql FROM sqlite_master WHERE name='note_items'");
        (void)q.step();
        return q.text(0);
    };
    const auto before = table_sql();
    {
        storage::SqliteTransaction tx(*database_);
        create_note_items_schema(*database_);
        create_note_items_schema(*database_);
        tx.commit();
    }
    // ADD COLUMN only inserts the column; the table and its CHECKs are not rebuilt.
    const auto schema = table_sql();
    auto without_column = schema;
    const auto added = without_column.find(", forum_id TEXT");
    ASSERT_NE(added, std::string::npos);
    without_column.erase(added, std::string_view(", forum_id TEXT").size());
    EXPECT_EQ(without_column, before);
    EXPECT_EQ(raw_content(*database_, note.id), blob);
    EXPECT_FALSE(note_find_item(*database_, note.id)->forum_id);
    database_->execute("INSERT INTO note_items(kind,name,forum_id) VALUES('folder','Forums','')");
    EXPECT_THROW(database_->execute("INSERT INTO note_items(kind,name,forum_id) VALUES('folder','Other','')"), std::runtime_error);
    EXPECT_THROW(database_->execute("INSERT INTO note_items(kind,name) VALUES('folder','ANNOTATIONS')"), std::runtime_error);
    EXPECT_NE(schema.find("name <> 'ANNOTATIONS'"), std::string::npos);
    EXPECT_TRUE(has_schema_object(*database_, "index", "note_items_forum_id"));
}

TEST_F(NoteStorageTest, ForumsPreserveLegacyFoldersAndAreIdempotent) {
    create_event_history_schema(*database_);
    const auto legacy = note_create(*database_, {}, NoteItemKind::folder, "Forums");
    const auto child = note_create(*database_, legacy.id, NoteItemKind::note, "Keep me");
    (void)note_create(*database_, {}, NoteItemKind::note, "Forums (saved)");
    (void)note_create(*database_, {}, NoteItemKind::folder, "Forums (saved) 1");
    const auto blob = raw_content(*database_, child.id);
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"a", "Forums"}, {"b", "Research"}});
        tx.commit();
    }
    const auto items = note_list(*database_);
    EXPECT_EQ(note_find_item(*database_, legacy.id)->name, "Forums (saved) 2");
    EXPECT_FALSE(note_find_item(*database_, legacy.id)->forum_id);
    EXPECT_EQ(note_find_item(*database_, child.id)->parent_id, legacy.id);
    EXPECT_EQ(raw_content(*database_, child.id), blob);
    ASSERT_EQ(event_list(*database_).size(), 1u);
    EXPECT_NE(event_list(*database_)[0].message.find("Forums (saved) 2"), std::string::npos);
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"a", "Forums"}, {"b", "Research"}});
        tx.commit();
    }
    EXPECT_EQ(nlohmann::json(note_list(*database_)), nlohmann::json(items));
    EXPECT_EQ(event_list(*database_).size(), 1u);
}

TEST_F(NoteStorageTest, ForumsPreserveLegacyNoteContentAndAnnotationsAtomically) {
    create_event_history_schema(*database_);
    const auto legacy = note_create(*database_, {}, NoteItemKind::note, "Forums");
    (void)note_save(*database_, legacy.id, nlohmann::json::parse(R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Saved highlight","marks":[{"type":"highlight"}]}]}]})"));
    note_save_compressed(*database_, legacy.id, "Compressed");
    const auto blob = raw_content(*database_, legacy.id);
    database_->execute("CREATE TRIGGER reject_forums BEFORE INSERT ON note_items WHEN NEW.forum_id='' BEGIN SELECT RAISE(ABORT,'test failure'); END");
    EXPECT_THROW({
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"a", "Research"}});
        tx.commit();
    }, std::runtime_error);
    EXPECT_EQ(note_find_item(*database_, legacy.id)->name, "Forums");
    EXPECT_TRUE(event_list(*database_).empty());
    database_->execute("DROP TRIGGER reject_forums");
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {});
        tx.commit();
    }
    EXPECT_EQ(note_find_item(*database_, legacy.id)->name, "Forums (saved)");
    EXPECT_EQ(raw_content(*database_, legacy.id), blob);
    EXPECT_EQ(note_get(*database_, legacy.id).compressed_text, "Compressed");
    const auto items = note_list(*database_);
    const auto annotation = std::ranges::find_if(items, [](const auto& i) { return i.kind == NoteItemKind::annotation; });
    ASSERT_NE(annotation, items.end());
    EXPECT_NE(note_get(*database_, annotation->id).document.dump().find("Forums (saved)"), std::string::npos);
}

TEST_F(NoteStorageTest, ForumsPreserveLegacyStoreAndEmptyFolderIdentity) {
    create_event_history_schema(*database_);
    create_folder_store_schema(*database_);
    const auto legacy = note_create(*database_, {}, NoteItemKind::folder, "Forums");
    const auto child = note_create(*database_, legacy.id, NoteItemKind::note, "Chapter");
    const auto store_id = folder_store_register(*database_, legacy.id, FolderStoreService::openai, "key", 1);
    folder_store_save_remote_id(*database_, store_id, "vs_saved");
    folder_store_add_file_id(*database_, store_id, "file_saved");
    folder_store_mark_ready(*database_, store_id);
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"a", "Research"}});
        tx.commit();
    }
    const auto saved = folder_store_get_by_folder(*database_, legacy.id);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->id, store_id);
    EXPECT_EQ(saved->remote_store_id, "vs_saved");
    EXPECT_EQ(saved->file_ids, std::vector<std::string>{"file_saved"});
    EXPECT_EQ(saved->state, FolderStoreState::ready);
    EXPECT_EQ(note_find_item(*database_, child.id)->parent_id, legacy.id);
    // An empty legacy folder is preserved, too, rather than adopted.
    database_->execute("DELETE FROM note_items WHERE forum_id IS NOT NULL");
    const auto empty = note_create(*database_, {}, NoteItemKind::folder, "Forums");
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {});
        tx.commit();
    }
    EXPECT_EQ(note_find_item(*database_, empty.id)->name, "Forums (saved) 1");
    EXPECT_FALSE(note_find_item(*database_, empty.id)->forum_id);
}

TEST_F(NoteStorageTest, ForumRemovalUsesSubtreeCleanupAndRollsBackFailedSwaps) {
    create_event_history_schema(*database_);
    std::int64_t folder = 0;
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"a", "Alpha"}, {"b", "Beta"}});
        tx.commit();
    }
    for (const auto& item : note_list(*database_)) if (item.forum_id == "a") folder = item.id;
    // Seed future generated content internally; user creation is forbidden.
    auto insert = database_->prepare("INSERT INTO note_items(parent_id,kind,name,content_json,read_only) VALUES (?,'note','Generated',?,1)");
    insert.bind(1, folder);
    insert.bind_blob(2, compressed_content(empty_note_document));
    insert.run();
    const auto child = database_->last_insert_rowid();
    (void)note_audio_accept(*database_, child, 1, "https://audio.test", "key", [](const auto&) {});
    const auto before = nlohmann::json(note_list(*database_));
    database_->execute("CREATE TRIGGER reject_final BEFORE UPDATE ON note_items WHEN NEW.name='Beta' BEGIN SELECT RAISE(ABORT,'test failure'); END");
    EXPECT_THROW({
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"a", "Beta"}, {"b", "Alpha"}});
        tx.commit();
    }, std::runtime_error);
    EXPECT_EQ(nlohmann::json(note_list(*database_)), before);
    database_->execute("DROP TRIGGER reject_final");
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"b", "Beta"}});
        tx.commit();
    }
    EXPECT_FALSE(note_find_item(*database_, folder));
    EXPECT_FALSE(note_find_item(*database_, child));
    EXPECT_TRUE(note_audio_attempts(*database_).empty());
    EXPECT_EQ(event_list(*database_).size(), 1u);
}

TEST_F(NoteStorageTest, ForumFoldersRejectUserMutationsAndSupportSwapsAndReusedIds) {
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"a", "Alpha"}, {"b", "Beta"}});
        tx.commit();
    }
    const auto original = note_list(*database_);
    for (const auto& item : original) {
        ASSERT_TRUE(item.forum_id);
        EXPECT_EQ(nlohmann::json(item).at("forum_id"), *item.forum_id);
        for (const auto kind : {NoteItemKind::folder, NoteItemKind::note}) {
            expect_note_error(NoteFailure::invalid, "This folder is managed by CHA.", [&] {
                (void)note_create(*database_, item.id, kind, "Child");
            });
        }
        expect_note_error(NoteFailure::invalid, "This folder is managed by CHA.", [&] {
            (void)note_import_book(*database_, item.id, "Book", {{"Chapter", nlohmann::json::object()}});
        });
        expect_note_error(NoteFailure::invalid, "This folder is managed by CHA.", [&] { (void)note_rename(*database_, item.id, item.name); });
        expect_note_error(NoteFailure::invalid, "This folder is managed by CHA.", [&] { note_delete(*database_, item.id); });
    }
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {{"a", "Beta"}, {"b", "Alpha"}});
        tx.commit();
    }
    for (const auto& item : original) {
        const auto now = *note_find_item(*database_, item.id);
        EXPECT_EQ(now.forum_id, item.forum_id);
        EXPECT_EQ(now.name, *item.forum_id == "a" ? "Beta" : *item.forum_id == "b" ? "Alpha" : "Forums");
    }
    {
        storage::SqliteTransaction tx(*database_);
        note_sync_forums(*database_, {});
        note_sync_forums(*database_, {{"a", "Alpha"}});
        tx.commit();
    }
    for (const auto& item : original) {
        if (!item.forum_id->empty()) EXPECT_FALSE(note_find_item(*database_, item.id));
    }
    (void)note_create(*database_, {}, NoteItemKind::folder, "forums");
    const auto ordinary = note_create(*database_, {}, NoteItemKind::folder, "Ordinary");
    (void)note_create(*database_, ordinary.id, NoteItemKind::folder, "Forums");
    EXPECT_FALSE(nlohmann::json(ordinary).contains("forum_id"));
}

TEST_F(NoteStorageTest, AudioReplacementIsAtomicPersistentAndCascadesWithNoteDeletion) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Narration");
    (void)note_set_read_only(*database_, note.id, true);
    const auto first = workspace_.root() / "first.m4a", second = workspace_.root() / "second.m4a";
    { std::ofstream(first, std::ios::binary) << "first clip"; std::ofstream(second, std::ios::binary) << "second clip"; }
    note_save_audio(*database_, note.id, {first, second});
    EXPECT_EQ(note_audio_info(*database_, note.id), (nlohmann::json{{"count", 2}}));
    EXPECT_EQ(note_audio_clip(*database_, note.id, 0).mime_type, "audio/mp4");
    EXPECT_THROW(note_save_audio(*database_, note.id, {second, workspace_.root() / "missing.m4a"}), std::exception);
    EXPECT_EQ(note_audio_clip(*database_, note.id, 0).bytes, "first clip");
    EXPECT_EQ(note_audio_clip(*database_, note.id, 1).bytes, "second clip");
    database_->execute("CREATE TRIGGER reject_audio BEFORE INSERT ON note_audio WHEN NEW.ordinal=1 BEGIN SELECT RAISE(ABORT, 'test insert failure'); END");
    EXPECT_THROW(note_save_audio(*database_, note.id, {second, first}), std::exception);
    EXPECT_EQ(note_audio_clip(*database_, note.id, 0).bytes, "first clip");
    database_->execute("DROP TRIGGER reject_audio");
    note_save_audio(*database_, note.id, {second});
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_EQ(note_audio_info(*database_, note.id)["count"], 1);
    EXPECT_EQ(note_audio_clip(*database_, note.id, 0).bytes, "second clip");
    EXPECT_THROW(note_audio_clip(*database_, note.id, 1), NoteError);
    note_delete(*database_, note.id);
    EXPECT_EQ(note_audio_info(*database_, note.id)["count"], 0);
    EXPECT_THROW(note_save_audio(*database_, note.id, {first}), NoteError);
}

TEST_F(NoteStorageTest, AudioSaveRequiresANormalReadOnlyNote) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Editable");
    const auto clip = workspace_.root() / "clip.m4a";
    std::ofstream(clip, std::ios::binary) << "clip";
    EXPECT_THROW(note_save_audio(*database_, note.id, {clip}), NoteError);
    (void)note_set_read_only(*database_, note.id, true);
    note_save_audio(*database_, note.id, {clip});
    (void)note_set_read_only(*database_, note.id, false);
    EXPECT_THROW(note_save_audio(*database_, note.id, {clip}), NoteError);
    EXPECT_EQ(note_audio_clip(*database_, note.id, 0).bytes, "clip");
}


nlohmann::json annotation_fixture() {
    std::ifstream input(std::filesystem::path(CHA_SOURCE_DIRECTORY) / "tests/fixtures/notes/annotations.json");
    return nlohmann::json::parse(input);
}
TEST_F(NoteStorageTest, AnnotationsLifecycleAndSharedRecognition) {
    const auto fixture = annotation_fixture();
    const auto source = note_create(*database_, {}, NoteItemKind::note, "Source");
    EXPECT_TRUE(note_save(*database_, source.id, fixture["document"]));
    const auto items = note_list(*database_);
    ASSERT_EQ(items.size(), 2u);
    const auto annotation = items.back();
    EXPECT_EQ(annotation.kind, NoteItemKind::annotation);
    EXPECT_EQ(annotation.name, "ANNOTATIONS");
    EXPECT_TRUE(annotation.read_only);
    const auto generated = note_get(*database_, annotation.id).document;
    EXPECT_EQ(generated, fixture["generated_document"]);
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& block : generated["content"]) {
        if (block["type"] != "paragraph") continue;
        auto attrs = block["content"][0]["attrs"];
        attrs.erase("sourceId");
        std::string text;
        for (const auto& node : block["content"]) {
            if (node["type"] == "text") text += node["text"].get<std::string>();
            if (node["type"] == "hardBreak") text += '\n';
        }
        attrs["text"] = text;
        entries.push_back(attrs);
    }
    EXPECT_EQ(entries, fixture["entries"]);
    EXPECT_EQ(generated["content"][2]["content"][0]["text"], "Chapter");
    note_save_scroll_position(*database_, annotation.id, 123);
    EXPECT_FALSE(note_save(*database_, source.id, fixture["document"]));
    EXPECT_EQ(note_get(*database_, annotation.id).scroll_top, 123);
    (void)note_rename(*database_, source.id, "Renamed");
    EXPECT_EQ(note_get(*database_, annotation.id).document["content"][0]["content"][0]["text"], "Renamed");
    for (const auto& operation : std::vector<std::function<void()>>{
        [&] { note_save(*database_, annotation.id, fixture["document"]); },
        [&] { (void)note_rename(*database_, annotation.id, "Other"); },
        [&] { note_delete(*database_, annotation.id); },
        [&] { (void)note_set_read_only(*database_, annotation.id, false); },
        [&] { note_save_compressed(*database_, annotation.id, "text"); }
    }) expect_note_error(NoteFailure::invalid, "That item is not a note.", operation);
    EXPECT_NE(note_get_text(*database_, annotation.id).text.find("Comment"), std::string::npos);
    note_delete(*database_, source.id);
    EXPECT_EQ(note_get(*database_, annotation.id).document["content"][0]["content"][0]["text"], "No comments or highlights.");
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_EQ(note_list(*database_).front().id, annotation.id);
}
TEST_F(NoteStorageTest, AnnotationReservedNamesScopeImportAndRollback) {
    for (const auto kind : {NoteItemKind::folder, NoteItemKind::note, NoteItemKind::annotation}) {
        EXPECT_THROW((void)note_create(*database_, {}, kind, "ANNOTATIONS"), NoteError);
    }
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Annotations");
    const auto source = note_create(*database_, folder.id, NoteItemKind::note, "Source");
    const auto doc = annotation_fixture()["document"];
    EXPECT_FALSE(note_save(*database_, source.id, nlohmann::json::parse(empty_note_document)));
    EXPECT_TRUE(note_save(*database_, source.id, doc));
    EXPECT_EQ(note_list(*database_).size(), 3u);
    database_->execute("CREATE TRIGGER fail_annotation BEFORE UPDATE ON note_items WHEN NEW.kind = 'annotation' BEGIN SELECT RAISE(ABORT, 'failed rebuild'); END");
    EXPECT_THROW((void)note_rename(*database_, source.id, "Rolled back"), NoteError);
    EXPECT_EQ(note_list(*database_)[1].name, "Source");
    EXPECT_THROW(note_delete(*database_, source.id), NoteError);
    EXPECT_THROW(note_save(*database_, source.id, nlohmann::json::parse(empty_note_document)), NoteError);
    EXPECT_EQ(note_get(*database_, source.id).document, doc);
    EXPECT_NO_THROW((void)note_get(*database_, source.id));
    database_->execute("DROP TRIGGER fail_annotation");
    note_delete(*database_, folder.id);
    EXPECT_TRUE(note_list(*database_).empty());
    const auto book = note_import_book(*database_, {}, "Book", {{"Chapter", doc}, {"Other", doc}});
    EXPECT_EQ(note_list(*database_).size(), 4u);
    EXPECT_EQ(note_list(*database_).back().parent_id, book);
}


TEST_F(NoteStorageTest, AnnotationExactByteLimitAndFreshBoundedReplacement) {
    using Json = nlohmann::json;
    const auto source = note_create(*database_, {}, NoteItemKind::note, "Source");
    Json document = {{"type", "doc"}, {"content", Json::array()}};
    Json expected = {{"type", "doc"}, {"content", Json::array({
        {{"type", "heading"}, {"attrs", {{"level", 1}}}, {"content", Json::array({{{"type", "text"}, {"text", "Source"}}})}}
    })}};
    std::size_t size = expected.dump().size();
    for (int ordinal = 1;; ++ordinal) {
        Json entry = {{"type", "paragraph"}, {"content", Json::array({
            {{"type", "annotationSource"}, {"attrs", {{"sourceId", source.id}, {"entryType", "highlight"}, {"ordinal", ordinal}}}},
            {{"type", "text"}, {"text", ""}}
        })}};
        const auto overhead = entry.dump().size() + 1;
        const auto count = std::min<std::size_t>(4000, 10000000 - size - overhead);
        entry["content"][1]["text"] = std::string(count, 'x');
        expected["content"].push_back(entry);
        size += overhead + count;
        document["content"].push_back({{"type", "paragraph"}, {"content", Json::array({
            {{"type", "text"}, {"text", std::string(count, 'x')}, {"marks", Json::array({{{"type", "highlight"}}})}}
        })}});
        if (size == 10000000) break;
    }
    document["content"].push_back(document["content"][0]);
    EXPECT_TRUE(note_save(*database_, source.id, document));
    const auto annotation = note_list(*database_).back();
    const auto generated = note_get(*database_, annotation.id).document;
    EXPECT_EQ(generated, expected);
    EXPECT_EQ(raw_content(*database_, annotation.id).size(), 10000000u);
    EXPECT_FALSE(note_save(*database_, source.id, annotation_fixture()["document"]));
    EXPECT_LT(raw_content(*database_, annotation.id).size(), 10000u);
}




TEST_F(NoteStorageTest, AnnotationCommentRunsShareLongChapterContext) {
    using Json = nlohmann::json;
    const auto source = note_create(*database_, {}, NoteItemKind::note, "Source");
    const std::string chapter(50000, 'c');
    Json document = {{"type", "doc"}, {"content", Json::array()}};
    for (int i = 0; i < 2000; ++i) document["content"].push_back({{"type", "paragraph"}, {"content", Json::array({
        {{"type", "text"}, {"text", "Comment"}, {"marks", Json::array({{{"type", "highlight"}, {"attrs", {{"color", "#b3d9ff"}}}}})}}
    })}});
    document["content"].push_back({{"type", "heading"}, {"attrs", {{"level", 1}}}, {"content", Json::array({{{"type", "text"}, {"text", chapter}}})}});
    EXPECT_TRUE(note_save(*database_, source.id, document));
    const auto generated = note_get(*database_, note_list(*database_).back().id).document;
    ASSERT_EQ(generated["content"].size(), 2002u);
    EXPECT_EQ(generated["content"][1]["content"][0]["text"], chapter);
    EXPECT_EQ(generated["content"].back()["content"][0]["attrs"]["ordinal"], 2000);
}

TEST_F(NoteStorageTest, AnnotationTruncationCountsUnicodeCharacters) {
    const auto source = note_create(*database_, {}, NoteItemKind::note, "Source");
    const auto text = repeated_code_point(4001);
    const auto doc = nlohmann::json::parse(R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"placeholder","marks":[{"type":"highlight"}]}]}]})");
    auto document = doc;
    document["content"][0]["content"][0]["text"] = text;
    EXPECT_TRUE(note_save(*database_, source.id, document));
    const auto generated = note_get(*database_, note_list(*database_).back().id).document;
    EXPECT_EQ(generated["content"][1]["content"][1]["text"], repeated_code_point(4000));
}

TEST_F(NoteStorageTest, AnnotationFirstCandidateTooLargeStoresEmptyDocumentAndWarns) {
    const auto log_path = workspace_.root() / "annotations.log";
    initialize_diagnostic_logging(log_path, "warn");
    struct StopLogging { ~StopLogging() { shutdown_diagnostic_logging(); } } stop_logging;
    const auto source = note_create(*database_, {}, NoteItemKind::note, "Source");
    auto document = nlohmann::json::parse(R"({"type":"doc","content":[
        {"type":"heading","attrs":{"level":2},"content":[{"type":"text","text":"Chapter"}]},
        {"type":"paragraph","content":[{"type":"text","text":"Entry","marks":[{"type":"highlight"}]}]},
        {"type":"paragraph","content":[{"type":"text","text":"Later","marks":[{"type":"highlight"}]}]}
    ]})");
    document["content"][0]["content"][0]["text"] = std::string(10000000, 'c');
    EXPECT_TRUE(note_save(*database_, source.id, document));
    const auto annotation = note_list(*database_).back();
    EXPECT_EQ(note_get(*database_, annotation.id).document, nlohmann::json::parse(empty_note_document));
    EXPECT_EQ(note_get(*database_, source.id).document, document);

    // A bounded update must replace existing entries, too.
    EXPECT_FALSE(note_save(*database_, source.id, annotation_fixture()["document"]));
    EXPECT_FALSE(note_save(*database_, source.id, document));
    EXPECT_EQ(note_get(*database_, annotation.id).document, nlohmann::json::parse(empty_note_document));
    shutdown_diagnostic_logging();
    std::ifstream input(log_path);
    const std::string log((std::istreambuf_iterator<char>(input)), {});
    const std::string warning = "ANNOTATIONS reached the 10 MB limit";
    const auto first = log.find(warning);
    ASSERT_NE(first, std::string::npos);
    const auto second = log.find(warning, first + warning.size());
    ASSERT_NE(second, std::string::npos);
    EXPECT_EQ(log.find(warning, second + warning.size()), std::string::npos);
}

TEST_F(NoteStorageTest, AnnotationExtractionToleratesUnusedFieldsAndNodeTypes) {
    const auto source = note_create(*database_, {}, NoteItemKind::note, "Source");
    const auto document = nlohmann::json::parse(R"({"type":"doc","content":[
        null, 7, {"type":7,"content":false}, {"type":"unknown","attrs":4},
        {"type":"paragraph","content":[{"type":"text","text":false,"marks":[null,{"type":5}]}]},
        {"type":"heading","attrs":false,"content":[{"type":"text","text":"Chapter"}]},
        {"type":"paragraph","content":[{"type":"text","text":"Entry","marks":[false,{"type":"highlight","attrs":7}]}]}
    ]})");
    EXPECT_TRUE(note_save(*database_, source.id, document));
    EXPECT_EQ(note_get(*database_, source.id).document, document);
    const auto generated = note_get(*database_, note_list(*database_).back().id).document;
    ASSERT_EQ(generated["content"].size(), 3u);
    EXPECT_EQ(generated["content"][2]["content"][1]["text"], "Entry");
}

TEST_F(NoteStorageTest, AnnotationNamesAndKindsAreReservedAtEveryWriteBoundary) {
    const auto source = note_create(*database_, {}, NoteItemKind::note, "Annotations");
    for (const auto kind : {NoteItemKind::note, NoteItemKind::folder}) {
        expect_note_error(NoteFailure::invalid, "The name ANNOTATIONS is reserved.", [&] {
            (void)note_create(*database_, {}, kind, "ANNOTATIONS");
        });
    }
    expect_note_error(NoteFailure::invalid, "Kind must be folder or note.", [&] {
        (void)note_create(*database_, {}, NoteItemKind::annotation, "Other");
    });
    expect_note_error(NoteFailure::invalid, "The name ANNOTATIONS is reserved.", [&] {
        (void)note_rename(*database_, source.id, "ANNOTATIONS");
    });
    const auto document = nlohmann::json::parse(empty_note_document);
    expect_note_error(NoteFailure::invalid, "The name ANNOTATIONS is reserved.", [&] {
        (void)note_import_book(*database_, {}, "ANNOTATIONS", {{"Chapter", document}});
    });
    expect_note_error(NoteFailure::invalid, "The name ANNOTATIONS is reserved.", [&] {
        (void)note_import_book(*database_, {}, "Book", {{"Chapter", document}, {"ANNOTATIONS", document}});
    });
    EXPECT_EQ(note_list(*database_).size(), 1u);
}

TEST_F(NoteStorageTest, AnnotationRebuildLargeBookAndIllustratedFolder) {
    using Json = nlohmann::json;
    const auto millis = [](auto started) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    };
    for (const bool illustrated : {false, true}) {
        Json document = {{"type", "doc"}, {"content", Json::array()}};
        for (int i = 0; i < (illustrated ? 1 : 200); ++i) document["content"].push_back({
            {"type", "paragraph"}, {"content", Json::array({{{"type", "text"}, {"text", std::string(4000, 'x')}, {"marks", Json::array({{{"type", "highlight"}}})}}})}
        });
        if (illustrated) document["content"].push_back({{"type", "image"}, {"attrs", {{"src", "data:image/png;base64," + std::string(12000000, 'A')}}}});
        std::vector<NoteImportEntry> notes;
        for (int i = 0; i < (illustrated ? 10 : 15); ++i) notes.push_back({"Chapter " + std::to_string(i), document});
        auto started = std::chrono::steady_clock::now();
        const auto folder = note_import_book(*database_, {}, illustrated ? "Illustrated" : "Book", notes);
        std::cout << (illustrated ? "Illustrated folder" : "Large book") << " import/rebuild: " << millis(started) << " ms\n";
        const auto listing = note_list(*database_);
        const auto source = std::find_if(listing.begin(), listing.end(), [&](const auto& item) { return item.parent_id == folder && item.kind == NoteItemKind::note; });
        (void)note_set_read_only(*database_, source->id, false);
        for (int repeat = 0; repeat < 2; ++repeat) {
            document["content"].push_back({{"type", "paragraph"}, {"content", Json::array({{{"type", "text"}, {"text", "ordinary text"}}})}});
            started = std::chrono::steady_clock::now();
            EXPECT_FALSE(note_save(*database_, source->id, document));
            std::cout << "  save/rebuild " << repeat + 1 << ": " << millis(started) << " ms\n";
            const auto generated = note_list(*database_).back();
            EXPECT_LE(raw_content(*database_, generated.id).size(), 10000000u);
        }
    }
}

TEST_F(NoteStorageTest, CompressionPreservesOriginalAndSurvivesEditsAndReopen) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Source");
    EXPECT_FALSE(note.has_compressed);
    EXPECT_FALSE(note_get(*database_, note.id).compressed_text.has_value());
    EXPECT_FALSE(nlohmann::json(note_get(*database_, note.id)).contains("compressed_text"));
    const auto original = raw_content(*database_, note.id);
    note_save_compressed(*database_, note.id, "First compressed text");
    EXPECT_EQ(note_get(*database_, note.id).compressed_text, "First compressed text");
    EXPECT_TRUE(note_list(*database_).front().has_compressed);
    EXPECT_EQ(raw_content(*database_, note.id), original);
    note_save(*database_, note.id, {{"type", "doc"}, {"changed", true}});
    EXPECT_TRUE(note_rename(*database_, note.id, "Renamed").has_compressed);
    note_save_compressed(*database_, note.id, "Replacement");
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_EQ(note_get(*database_, note.id).compressed_text, "Replacement");
    EXPECT_EQ(nlohmann::json(note_get(*database_, note.id))["compressed_text"], "Replacement");
    auto stored = database_->prepare("SELECT compressed_text FROM note_items WHERE id = ?1", note.id);
    ASSERT_TRUE(stored.step());
    EXPECT_EQ(stored.text(0), "Replacement");
    EXPECT_THROW(note_save_compressed(*database_, note.id, " \n\t"), NoteError);
}

TEST_F(NoteStorageTest, CompressionCommitChecksSourceAndAtomicallyStoresItsEvent) {
    create_event_history_schema(*database_);
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Source");
    const auto source = note_get_text(*database_, note.id).text;
    note_save_compressed(*database_, note.id, "Previous");
    EXPECT_FALSE(note_commit_compressed(*database_, note.id, "Changed", "Stale", "Ready"));
    EXPECT_EQ(note_get(*database_, note.id).compressed_text, "Previous");
    EXPECT_TRUE(event_list(*database_).empty());
    database_->execute("CREATE TRIGGER reject_success BEFORE INSERT ON event_history BEGIN SELECT RAISE(ABORT,'event failure'); END");
    EXPECT_THROW(note_commit_compressed(*database_, note.id, source, "Replacement", "Ready"), NoteError);
    EXPECT_EQ(note_get(*database_, note.id).compressed_text, "Previous");
    EXPECT_TRUE(event_list(*database_).empty());
    database_->execute("DROP TRIGGER reject_success");
    EXPECT_TRUE(note_commit_compressed(*database_, note.id, source, "Replacement", "Ready"));
    EXPECT_EQ(note_get(*database_, note.id).compressed_text, "Replacement");
    ASSERT_EQ(event_list(*database_).size(), 1U);
    EXPECT_EQ(event_list(*database_).front().severity, EventSeverity::success);
}

TEST_F(NoteStorageTest, CompressionSourceIncludesCommentsButIgnoresUnchangedHighlights) {
    create_event_history_schema(*database_);
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Source");
    auto document = nlohmann::json::parse(R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Original"}]}]})");
    note_save(*database_, note.id, document);
    const auto source = note_get_text(*database_, note.id).text;
    document["content"][0]["content"][0]["marks"] = nlohmann::json::parse(R"([{"type":"highlight","attrs":{"color":"yellow"}}])");
    note_save(*database_, note.id, document);
    EXPECT_TRUE(note_commit_compressed(*database_, note.id, source, "Summary", "Ready"));
    document["content"].push_back(nlohmann::json::parse(R"({"type":"paragraph","content":[{"type":"text","text":"Comment","marks":[{"type":"highlight","attrs":{"color":"#b3d9ff"}}]}]})"));
    note_save(*database_, note.id, document);
    EXPECT_FALSE(note_commit_compressed(*database_, note.id, source, "Stale", "Ready"));
    EXPECT_EQ(note_get(*database_, note.id).compressed_text, "Summary");
    EXPECT_EQ(event_list(*database_).size(), 1U);
}

TEST_F(NoteStorageTest, ReadOnlyStateSurvivesReopenAndProtectsOnlyContent) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Source");
    EXPECT_FALSE(note.read_only);
    const auto original = note_get(*database_, note.id).document;
    EXPECT_TRUE(note_set_read_only(*database_, note.id, true).read_only);
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_TRUE(note_list(*database_).front().read_only);
    EXPECT_EQ(nlohmann::json(note_list(*database_).front())["read_only"], true);
    expect_note_error(NoteFailure::invalid, "This note is read-only. Make it editable first.", [&] {
        note_save(*database_, note.id, {{"changed", true}});
    });
    EXPECT_EQ(note_get(*database_, note.id).document, original);
    EXPECT_TRUE(note_rename(*database_, note.id, "Renamed").read_only);
    note_save_compressed(*database_, note.id, "Summary");
    EXPECT_EQ(note_get(*database_, note.id).compressed_text, "Summary");
    EXPECT_EQ(note_get_text(*database_, note.id, true).text, "Summary");
    EXPECT_FALSE(note_set_read_only(*database_, note.id, false).read_only);
    note_save(*database_, note.id, {{"changed", true}});
    EXPECT_EQ(note_get(*database_, note.id).document, nlohmann::json({{"changed", true}}));
    EXPECT_TRUE(note_set_read_only(*database_, note.id, true).read_only);
    note_delete(*database_, note.id);
    EXPECT_TRUE(note_list(*database_).empty());
    EXPECT_THROW((void)note_set_read_only(*database_, note.id, false), NoteError);
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Folder");
    EXPECT_THROW((void)note_set_read_only(*database_, folder.id, true), NoteError);
}

TEST_F(NoteStorageTest, ReadOnlyNotesSaveHighlightsButRejectOtherChanges) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Reading");
    const auto original = nlohmann::json::parse(R"({"type":"doc","content":[
        {"type":"paragraph","content":[
            {"type":"text","text":"Alpha beta","marks":[{"type":"bold"}]},
            {"type":"text","text":" link","marks":[{"type":"link","attrs":{"href":"https://example.com"}}]}
        ]},
        {"type":"image","attrs":{"src":"data:image/png;base64,aaaa"}}
    ]})");
    note_save(*database_, note.id, original);
    (void)note_set_read_only(*database_, note.id, true);
    auto highlighted = original;
    highlighted["content"][0]["content"] = nlohmann::json::parse(R"([
        {"type":"text","text":"A","marks":[{"type":"bold"}]},
        {"type":"text","text":"lph","marks":[{"type":"bold"},{"type":"highlight"}]},
        {"type":"text","text":"a beta","marks":[{"type":"bold"}]},
        {"type":"text","text":" link","marks":[{"type":"link","attrs":{"href":"https://example.com"}},{"type":"highlight"}]}
    ])");
    note_save(*database_, note.id, highlighted);
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_EQ(note_get(*database_, note.id).document, highlighted);
    EXPECT_TRUE(note_list(*database_).front().read_only);

    auto changed = highlighted;
    changed["content"][0]["content"][1]["text"] = "Changed";
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    changed = highlighted;
    changed["content"][0]["content"][1]["marks"][0]["type"] = "italic";
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    changed = highlighted;
    changed["content"][0]["content"][3]["marks"][0]["attrs"]["href"] = "https://other.example.com";
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    changed = highlighted;
    changed["content"][1]["attrs"]["src"] = "data:image/png;base64,bbbb";
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    changed = highlighted;
    changed["content"][0]["type"] = "heading";
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    EXPECT_EQ(note_get(*database_, note.id).document, highlighted);

    note_save(*database_, note.id, original);
    EXPECT_EQ(note_get(*database_, note.id).document, original);
}

TEST_F(NoteStorageTest, ReadOnlyNotesSaveCommentAdditionsAndDeletionsButKeepNoteContent) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Reading");
    const auto original = nlohmann::json::parse(R"({"type":"doc","content":[
        {"type":"paragraph","content":[{"type":"text","text":"Alpha"}]},
        {"type":"bulletList","content":[{"type":"listItem","content":[
            {"type":"paragraph","content":[{"type":"text","text":"Nested"}]}
        ]}]},
        {"type":"paragraph","content":[{"type":"text","text":"Original highlighted text",
            "marks":[{"type":"highlight","attrs":{"color":"yellow"}}]}]}
    ]})");
    const auto comment = nlohmann::json::parse(R"({"type":"paragraph","content":[
        {"type":"text","text":"My comment","marks":[{"type":"highlight","attrs":{"color":"#b3d9ff"}}]},
        {"type":"hardBreak"},
        {"type":"text","text":"Second line","marks":[{"type":"highlight","attrs":{"color":"#b3d9ff"}}]}
    ]})");
    note_save(*database_, note.id, original);
    (void)note_set_read_only(*database_, note.id, true);
    auto commented = original;
    auto& blocks = commented["content"].get_ref<nlohmann::json::array_t&>();
    blocks.insert(blocks.begin(), comment);
    auto& nested = commented["content"][2]["content"][0]["content"].get_ref<nlohmann::json::array_t&>();
    nested.insert(nested.begin(), comment);
    note_save(*database_, note.id, commented);
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_EQ(note_get(*database_, note.id).document, commented);
    EXPECT_TRUE(note_list(*database_).front().read_only);

    auto changed = commented;
    changed["content"][1]["content"][0]["text"] = "Changed original";
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    changed = commented;
    changed["content"].erase(3); // Ordinary highlighted paragraphs are protected too.
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    changed = commented;
    auto unhighlighted = comment;
    unhighlighted["content"][0]["marks"][0]["attrs"]["color"] = "yellow";
    unhighlighted["content"][0]["text"] = "Different unhighlighted text";
    auto& changed_blocks = changed["content"].get_ref<nlohmann::json::array_t&>();
    changed_blocks.insert(changed_blocks.begin(), unhighlighted);
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    EXPECT_EQ(note_get(*database_, note.id).document, commented);
    note_save(*database_, note.id, original); // Delete both the top-level and nested comments.
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_EQ(note_get(*database_, note.id).document, original);
    EXPECT_TRUE(note_list(*database_).front().read_only);

    auto repeated = original;
    auto duplicate = comment;
    duplicate["content"] = nlohmann::json::array({duplicate["content"][0]});
    duplicate["content"][0]["text"] = "Alpha";
    auto& repeated_blocks = repeated["content"].get_ref<nlohmann::json::array_t&>();
    repeated_blocks.insert(repeated_blocks.begin() + 1, duplicate);
    repeated_blocks.insert(repeated_blocks.begin(), duplicate);
    const auto second = note_create(*database_, {}, NoteItemKind::note, "Repeated text");
    note_save(*database_, second.id, original);
    (void)note_set_read_only(*database_, second.id, true);
    note_save(*database_, second.id, repeated);
    EXPECT_EQ(note_get(*database_, second.id).document, repeated);
    note_save(*database_, second.id, original);
    EXPECT_EQ(note_get(*database_, second.id).document, original);

    const auto third = note_create(*database_, {}, NoteItemKind::note, "Only comment");
    note_save(*database_, third.id, {{"type", "doc"}, {"content", nlohmann::json::array({comment})}});
    (void)note_set_read_only(*database_, third.id, true);
    const nlohmann::json empty = {{"type", "doc"}, {"content", nlohmann::json::array({{{"type", "paragraph"}}})}};
    note_save(*database_, third.id, empty);
    EXPECT_EQ(note_get(*database_, third.id).document, empty);
}

TEST_F(NoteStorageTest, ScrollPositionsArePerNoteAndSurviveReadOnlyAndReopen) {
    const auto first = note_create(*database_, {}, NoteItemKind::note, "First");
    const auto second = note_create(*database_, {}, NoteItemKind::note, "Second");
    EXPECT_EQ(note_get(*database_, first.id).scroll_top, 0);
    const auto original = raw_content(*database_, first.id);
    note_save_scroll_position(*database_, first.id, 1250);
    (void)note_set_read_only(*database_, second.id, true);
    note_save_scroll_position(*database_, second.id, 750);
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_EQ(note_get(*database_, first.id).scroll_top, 1250);
    EXPECT_EQ(note_get(*database_, second.id).scroll_top, 750);
    EXPECT_EQ(nlohmann::json(note_get(*database_, second.id))["scroll_top"], 750);
    EXPECT_EQ(raw_content(*database_, first.id), original);
    note_save_scroll_position(*database_, first.id, 0);
    EXPECT_EQ(note_get(*database_, first.id).scroll_top, 0);
    EXPECT_THROW(note_save_scroll_position(*database_, first.id, -1), NoteError);
    EXPECT_THROW(note_save_scroll_position(*database_, first.id, note_id_limit + 1), NoteError);
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Folder");
    expect_note_error(NoteFailure::invalid, "That item is not a note.", [&] {
        note_save_scroll_position(*database_, folder.id, 10);
    });
    note_delete(*database_, first.id);
    expect_note_error(NoteFailure::not_found, "That note was not found.", [&] {
        note_save_scroll_position(*database_, first.id, 10);
    });
}

TEST_F(NoteStorageTest, NewDatabaseKeepsSchema2IdentityAndNoteSchema) {
    EXPECT_EQ(
        database_->pragma_integer("user_version"),
        workspace_session_database_version);
    EXPECT_EQ(
        database_->pragma_integer("application_id"),
        workspace_session_application_id);
    EXPECT_TRUE(has_schema_object(*database_, "table", "note_items"));
    EXPECT_TRUE(has_schema_object(*database_, "index", "note_items_sibling_name"));
    EXPECT_TRUE(has_schema_object(*database_, "index", "note_items_parent"));
    EXPECT_NO_THROW(validate_workspace_session_contents(*database_));
    EXPECT_TRUE(note_list(*database_).empty());

    EXPECT_THROW(
        database_->execute(
            "INSERT INTO note_items (id, kind, name, content_json) "
            "VALUES (0, 'note', 'Zero', '{}')"),
        std::runtime_error);
    EXPECT_THROW(
        database_->execute(
            "INSERT INTO note_items (id, kind, name, content_json) "
            "VALUES (9007199254740992, 'note', 'Big', '{}')"),
        std::runtime_error);
    EXPECT_THROW(
        database_->execute(
            "INSERT INTO note_items (kind, name, content_json) "
            "VALUES ('file', 'Bad', NULL)"),
        std::runtime_error);
    EXPECT_THROW(
        database_->execute(
            "INSERT INTO note_items (kind, name, content_json) "
            "VALUES ('folder', 'Body', '{}')"),
        std::runtime_error);
    EXPECT_THROW(
        database_->execute(
            "INSERT INTO note_items (kind, name, content_json) "
            "VALUES ('note', 'Empty', NULL)"),
        std::runtime_error);
    database_->execute(
        "INSERT INTO note_items (kind, name, content_json) "
        "VALUES ('folder', 'Plan', NULL)");
    EXPECT_THROW(
        database_->execute(
            "INSERT INTO note_items (kind, name, content_json) "
            "VALUES ('note', 'Plan', '{}')"),
        std::runtime_error);
}

TEST_F(NoteStorageTest, StoresCompressedBlobAndReadsItAfterReopen) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Chapter");
    const nlohmann::json document = {{"type", "doc"}, {"content", {
        {{"type", "paragraph"}, {"content", {{{"type", "text"}, {"text", std::string(100000, 'a')}}}}}
    }}};
    note_save(*database_, note.id, document);
    {
        auto stored = database_->prepare("SELECT typeof(content_json), length(content_json) FROM note_items WHERE id = ?1", note.id);
        ASSERT_TRUE(stored.step());
        EXPECT_EQ(stored.text(0), "blob");
        EXPECT_LT(stored.integer(1), 10000);
    }
    database_.reset();
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    EXPECT_EQ(note_get(*database_, note.id).document, document);
    EXPECT_EQ(note_get_text(*database_, note.id).text, std::string(100000, 'a'));
    auto damaged = database_->prepare("UPDATE note_items SET content_json = ?1 WHERE id = ?2");
    damaged.bind_blob(1, "broken");
    damaged.bind(2, note.id);
    damaged.run();
    expect_note_error(NoteFailure::opening, "The note could not be opened.", [&] {
        (void)note_get(*database_, note.id);
    });
}

TEST_F(NoteStorageTest, ReplacesLegacyTextNotesOnce) {
    database_->execute(R"sql(
        DROP TABLE note_items;
        CREATE TABLE note_items (
            id INTEGER PRIMARY KEY,
            parent_id INTEGER REFERENCES note_items(id) ON DELETE CASCADE,
            content_json TEXT
        ) STRICT;
        INSERT INTO note_items VALUES (1, NULL, NULL), (2, 1, NULL), (3, 2, '{}');
    )sql");
    {
        storage::SqliteTransaction transaction(*database_);
        create_note_items_schema(*database_);
        transaction.commit();
    }
    EXPECT_TRUE(note_list(*database_).empty());
    const auto note = note_create(*database_, {}, NoteItemKind::note, "New note");
    {
        storage::SqliteTransaction transaction(*database_);
        create_note_items_schema(*database_);
        transaction.commit();
    }
    ASSERT_EQ(note_list(*database_).size(), 1U);
    EXPECT_EQ(note_get(*database_, note.id).document, nlohmann::json::parse(empty_note_document));
}

TEST(NoteStorage, ConvertsNoteBlocksLinksAndImageDescriptionsToMarkdown) {
    const auto document = nlohmann::json::parse(R"json({"type":"doc","content":[
        {"type":"heading","attrs":{"level":2},"content":[{"type":"text","text":"Heading"}]},
        {"type":"paragraph","content":[{"type":"text","text":"Read "},
            {"type":"text","text":"the source","marks":[{"type":"link","attrs":{"href":"https://example.com"}}]},
            {"type":"hardBreak"},{"type":"text","text":"Next line"}]},
        {"type":"bulletList","content":[
            {"type":"listItem","content":[{"type":"paragraph","content":[{"type":"text","text":"Item"}]}]}]},
        {"type":"codeBlock","content":[{"type":"text","text":"print(1)\nprint(2)"}]},
        {"type":"image","attrs":{"src":"data:image/png;base64,private-bytes","alt":"A diagram"}},
        {"type":"image","attrs":{"src":"data:image/png;base64,private-bytes","alt":null}}
    ]})json");
    EXPECT_EQ(note_document_markdown(document),
        "### Heading\n\nRead [the source](<https://example.com>)\\\nNext line\n\n- Item\n\n"
        "```\nprint(1)\nprint(2)\n```\n\nA diagram");
    EXPECT_EQ(note_document_markdown(nlohmann::json::parse(empty_note_document)), "");
    EXPECT_EQ(note_document_markdown(nlohmann::json::parse(
        R"({"type":"doc","content":[{"type":"codeBlock","content":[{"type":"text","text":"    indented code"}]}]})")),
        "```\n    indented code\n```");
    EXPECT_EQ(note_document_markdown(nlohmann::json::parse(
        R"({"type":"doc","content":[{"type":"codeBlock","content":[{"type":"text","text":"print(1)\n"}]}]})")),
        "```\nprint(1)\n```");
}

TEST(NoteStorage, KeepsNestedListsNumberingAndParagraphsInMarkdown) {
    const auto document = nlohmann::json::parse(R"json({"type":"doc","content":[
        {"type":"orderedList","attrs":{"start":4},"content":[
            {"type":"listItem","content":[
                {"type":"paragraph","content":[{"type":"text","text":"First"}]},
                {"type":"bulletList","content":[{"type":"listItem","content":[
                    {"type":"paragraph","content":[{"type":"text","text":"Nested"}]},
                    {"type":"orderedList","attrs":{"start":2},"content":[{"type":"listItem","content":[
                        {"type":"paragraph","content":[{"type":"text","text":"Deep"}]}]}]}
                ]}]}]},
            {"type":"listItem","content":[
                {"type":"paragraph","content":[{"type":"text","text":"Second"}]},
                {"type":"paragraph","content":[{"type":"text","text":"Continuation"}]}]}
        ]}
    ]})json");
    EXPECT_EQ(note_document_markdown(document),
        "4. First\n\n   - Nested\n\n     2. Deep\n"
        "5. Second\n\n   Continuation");
}

TEST(NoteStorage, KeepsLiteralTextAndPreservesInlineMarksAndCodeFences) {
    const auto document = nlohmann::json::parse(R"json({"type":"doc","content":[
        {"type":"paragraph","content":[
            {"type":"text","text":"**literal** [text] &lt;\n1. literal list\n"},
            {"type":"text","text":"Bold ","marks":[{"type":"bold"}]},
            {"type":"text","text":"italic","marks":[{"type":"italic"}]},
            {"type":"text","text":" label ","marks":[{"type":"link","attrs":{"href":"https://example.com/a(b)?x=<value>&copy;=a b"}}]}
        ]},
        {"type":"codeBlock","attrs":{"language":"python"},"content":[{"type":"text","text":"    x = '**literal**'\n```"}]}
    ]})json");
    EXPECT_EQ(note_document_markdown(document),
        "**literal** [text] &lt;\n1. literal list\n"
        "**Bold** *italic*[ label ](<https://example.com/a(b)?x=%3Cvalue%3E&copy;=a%20b>)"
        "\n\n````python\n    x = '**literal**'\n```\n````");
}

TEST(NoteStorage, PreservesInlineCodeAndLiteralBackticksInChatMarkdown) {
    for (const auto& [text, expected] : std::vector<std::pair<std::string, std::string>>{
        {"query()", "`query()`"},
        {"a`b", "``a`b``"},
        {"`literal`", "`` `literal` ``"},
        {" a ", "`  a  `"},
        {"  ", "`  `"},
    }) {
        auto document = nlohmann::json::parse(R"({"type":"doc","content":[{"type":"paragraph","content":[
            {"type":"text","text":"Before "},
            {"type":"text","text":"","marks":[{"type":"code"}]},
            {"type":"text","text":" after"}]}]})");
        document["content"][0]["content"][1]["text"] = text;
        EXPECT_EQ(note_document_markdown(document), "Before " + expected + " after");
    }
}

TEST(NoteStorage, SessionLabelErrorsStayUnchanged) {
    expect_invalid_argument("Session label must not be empty", [] {
        validate_session_label("");
    });
    expect_invalid_argument("Session label must be valid UTF-8", [] {
        validate_session_label("\xff");
    });
    expect_invalid_argument(
        "Session label cannot contain a control character or line break",
        [] { validate_session_label("a\nb"); });
    expect_invalid_argument(
        "Session label cannot exceed 200 Unicode characters",
        [] { validate_session_label(std::string(201, 'a')); });
    expect_invalid_argument(
        "Session label cannot start or end with whitespace",
        [] { validate_session_label(" label"); });
    EXPECT_NO_THROW(validate_session_label(std::string(200, 'a')));
}

TEST_F(NoteStorageTest, NamesParentsAndSiblingNamespace) {
    expect_note_error(NoteFailure::invalid, "Name must not be empty", [&] {
        (void)note_create(*database_, std::nullopt, NoteItemKind::folder, "");
    });
    expect_note_error(NoteFailure::invalid, "Name must be valid UTF-8", [&] {
        (void)note_create(*database_, std::nullopt, NoteItemKind::folder, "\xff");
    });
    expect_note_error(
        NoteFailure::invalid,
        "Name cannot contain a control character or line break",
        [&] {
            (void)note_create(*database_, std::nullopt, NoteItemKind::note, "a\nb");
        });
    expect_note_error(
        NoteFailure::invalid,
        "Name cannot exceed 200 Unicode characters",
        [&] {
            (void)note_create(
                *database_, std::nullopt, NoteItemKind::folder, std::string(201, 'a'));
        });
    expect_note_error(
        NoteFailure::invalid,
        "Name cannot start or end with whitespace",
        [&] {
            (void)note_create(*database_, std::nullopt, NoteItemKind::folder, "Plan ");
        });
    EXPECT_EQ(item_count(*database_), 0);

    const auto wide = note_create(
        *database_, std::nullopt, NoteItemKind::folder, repeated_code_point(200));
    EXPECT_EQ(wide.name, repeated_code_point(200));
    const auto slash = note_create(
        *database_, std::nullopt, NoteItemKind::note, "plans/today:now");
    EXPECT_EQ(raw_content(*database_, slash.id), std::string(empty_note_document));

    const auto folder = note_create(
        *database_, std::nullopt, NoteItemKind::folder, "Plan");
    const auto other_case = note_create(
        *database_, std::nullopt, NoteItemKind::note, "plan");
    EXPECT_NE(folder.id, other_case.id);
    const auto nested = note_create(
        *database_, folder.id, NoteItemKind::folder, "Plan");
    const auto same_elsewhere = note_create(
        *database_, nested.id, NoteItemKind::note, "plan");
    EXPECT_EQ(same_elsewhere.parent_id, nested.id);

    expect_note_error(
        NoteFailure::invalid,
        "An item with this name already exists here.",
        [&] {
            (void)note_create(*database_, std::nullopt, NoteItemKind::folder, "Plan");
        });
    expect_note_error(
        NoteFailure::invalid,
        "An item with this name already exists here.",
        [&] {
            (void)note_create(*database_, folder.id, NoteItemKind::note, "Plan");
        });
    EXPECT_EQ(item_count(*database_), 6);

    expect_note_error(
        NoteFailure::invalid,
        "The parent folder was not found.",
        [&] {
            (void)note_create(*database_, 999, NoteItemKind::note, "Missing");
        });
    expect_note_error(
        NoteFailure::invalid,
        "The parent must be a folder.",
        [&] {
            (void)note_create(*database_, other_case.id, NoteItemKind::note, "Child");
        });
    EXPECT_EQ(item_count(*database_), 6);

    const auto renamed = note_rename(*database_, slash.id, "plans/today:now");
    EXPECT_EQ(renamed.name, "plans/today:now");
    EXPECT_EQ(raw_content(*database_, slash.id), std::string(empty_note_document));
    Statement spaced = database_->prepare(
        "UPDATE note_items SET content_json = ?1 WHERE id = ?2");
    const std::string spaced_json = "{ \"type\" : \"doc\" }";
    spaced.bind_blob(1, compressed_content(spaced_json));
    spaced.bind(2, slash.id);
    spaced.run();
    const auto next_name = note_rename(*database_, slash.id, "Renamed");
    EXPECT_EQ(next_name.name, "Renamed");
    EXPECT_EQ(raw_content(*database_, slash.id), spaced_json);
    expect_note_error(
        NoteFailure::invalid,
        "An item with this name already exists here.",
        [&] { (void)note_rename(*database_, slash.id, "Plan"); });
    EXPECT_EQ(note_list(*database_).size(), 6U);
    const auto still = note_list(*database_);
    const auto found = std::find_if(still.begin(), still.end(), [&](const NoteItem& item) {
        return item.id == folder.id;
    });
    ASSERT_NE(found, still.end());
    EXPECT_EQ(found->parent_id, std::nullopt);
    EXPECT_EQ(found->name, "Plan");
}

TEST_F(NoteStorageTest, BookImportCommitsReadOnlyNotesAndRejectsCollisions) {
    const auto document = nlohmann::json::parse(empty_note_document);
    const auto folder = note_import_book(*database_, std::nullopt, "Book", {
        {"001. Introduction", document}, {"002. Introduction", document},
    });
    const auto items = note_list(*database_);
    ASSERT_EQ(items.size(), 3U);
    EXPECT_EQ(items[0].id, folder);
    EXPECT_EQ(items[0].parent_id, std::nullopt);
    EXPECT_EQ(items[0].kind, NoteItemKind::folder);
    EXPECT_EQ(items[1].parent_id, folder);
    EXPECT_EQ(items[2].parent_id, folder);
    EXPECT_FALSE(items[0].read_only);
    EXPECT_TRUE(items[1].read_only);
    EXPECT_TRUE(items[2].read_only);
    EXPECT_EQ(note_get(*database_, items[1].id).document, document);
    auto edited = document;
    edited["content"][0]["content"] = {{{"type", "text"}, {"text", "Edited"}}};
    EXPECT_THROW(note_save(*database_, items[1].id, edited), NoteError);
    EXPECT_FALSE(note_set_read_only(*database_, items[1].id, false).read_only);
    note_save(*database_, items[1].id, edited);
    EXPECT_EQ(note_get(*database_, items[1].id).document, edited);
    expect_note_error(NoteFailure::invalid, "An item with this name already exists here.", [&] {
        (void)note_import_book(*database_, std::nullopt, "Book", {{"001. Other", document}});
    });
    EXPECT_EQ(item_count(*database_), 3);
}

TEST_F(NoteStorageTest, BookImportRollsBackOnInvalidNamesDocumentsAndInsertFailures) {
    const auto document = nlohmann::json::parse(empty_note_document);
    const NoteImportEntry valid{"001. Good", document};
    for (const auto& invalid : std::vector<NoteImportEntry>{
            {"bad\nname", document},
            {"002. Bad document", nlohmann::json::array()},
            valid}) {
        EXPECT_THROW((void)note_import_book(*database_, std::nullopt, "Book", {valid, invalid}), NoteError);
        EXPECT_EQ(item_count(*database_), 0);
    }
    auto oversized = document;
    oversized["padding"] = std::string(note_document_byte_limit, 'x');
    EXPECT_THROW((void)note_import_book(*database_, std::nullopt, "Book", {valid, {"002. Large", oversized}}), NoteError);
    EXPECT_EQ(item_count(*database_), 0);
    EXPECT_THROW((void)note_import_book(*database_, std::nullopt, "", {valid}), NoteError);
    EXPECT_THROW((void)note_import_book(*database_, std::nullopt, "Book", {}), NoteError);
    database_->execute(
        "CREATE TRIGGER fail_book_note BEFORE INSERT ON note_items "
        "WHEN NEW.name = '002. Fail' BEGIN SELECT RAISE(ABORT, 'test insert failure'); END");
    EXPECT_THROW((void)note_import_book(*database_, std::nullopt, "Book", {valid, {"002. Fail", document}}), NoteError);
    EXPECT_EQ(item_count(*database_), 0);
    database_->execute("DROP TRIGGER fail_book_note");
    EXPECT_GT(note_import_book(*database_, std::nullopt, "Book", {valid}), 0);
    EXPECT_EQ(item_count(*database_), 2);
}

TEST_F(NoteStorageTest, BookImportUsesDestinationFolderAndValidatesItBeforeWriting) {
    const auto document = nlohmann::json::parse(empty_note_document);
    const std::vector<NoteImportEntry> notes{{"001. First", document}};
    const auto parent = note_create(*database_, std::nullopt, NoteItemKind::folder, "Research");
    const auto nested = note_create(*database_, parent.id, NoteItemKind::folder, "Fiction");
    (void)note_import_book(*database_, std::nullopt, "Book", notes);
    const auto imported = note_import_book(*database_, nested.id, "Book", notes);
    const auto items = note_list(*database_);
    const auto book = std::find_if(items.begin(), items.end(), [&](const NoteItem& item) {
        return item.id == imported;
    });
    ASSERT_NE(book, items.end());
    EXPECT_EQ(book->parent_id, nested.id);
    const auto chapter = std::find_if(items.begin(), items.end(), [&](const NoteItem& item) {
        return item.parent_id == imported;
    });
    ASSERT_NE(chapter, items.end());
    EXPECT_EQ(note_get(*database_, chapter->id).document, document);
    const auto count = item_count(*database_);
    EXPECT_THROW((void)note_import_book(*database_, nested.id, "Book", notes), NoteError);
    EXPECT_THROW((void)note_import_book(*database_, chapter->id, "Other", notes), NoteError);
    EXPECT_THROW((void)note_import_book(*database_, 99999, "Other", notes), NoteError);
    EXPECT_EQ(item_count(*database_), count);
}

TEST_F(NoteStorageTest, DeleteCascadesAndDoesNotReuseIds) {
    const auto root = note_create(*database_, std::nullopt, NoteItemKind::folder, "Root");
    const auto child = note_create(*database_, root.id, NoteItemKind::folder, "Child");
    const auto grandchild = note_create(
        *database_, child.id, NoteItemKind::note, "Grandchild");
    const auto sibling = note_create(
        *database_, std::nullopt, NoteItemKind::note, "Sibling");
    database_->execute("INSERT INTO forums (forum_id) VALUES ('refs')");
    database_->execute("INSERT INTO sessions (forum_key,session_id,label,updated_at,history_epoch,next_entry_id,next_request_id) VALUES (1,'s','S',0,1,1,1)");
    session_folder_insert(*database_, 1, root.id);
    session_folder_insert(*database_, 1, child.id);
    note_delete(*database_, root.id);
    EXPECT_TRUE(session_folder_ids(*database_, 1).empty());
    const auto remaining = note_list(*database_);
    ASSERT_EQ(remaining.size(), 1U);
    EXPECT_EQ(remaining.front().id, sibling.id);
    EXPECT_EQ(note_sequence(*database_), sibling.id);
    expect_note_error(NoteFailure::not_found, "That note was not found.", [&] {
        note_save(*database_, grandchild.id, nlohmann::json::object());
    });
    expect_note_error(
        NoteFailure::not_found,
        "That note or folder was not found.",
        [&] { note_delete(*database_, grandchild.id); });
    const auto created = note_create(
        *database_, std::nullopt, NoteItemKind::folder, "Next");
    EXPECT_GT(created.id, grandchild.id);
    EXPECT_EQ(note_sequence(*database_), created.id);
    EXPECT_EQ(item_count(*database_), 2);
}

TEST_F(NoteStorageTest, DamagedRowUsesTheOperationMessage) {
    database_->execute("PRAGMA ignore_check_constraints = ON");
    database_->execute(
        "INSERT INTO note_items (id, kind, name, content_json) "
        "VALUES (1, 'file', 'Damaged', NULL)");
    database_->execute("PRAGMA ignore_check_constraints = OFF");
    expect_note_error(NoteFailure::opening, "The notes could not be loaded.", [&] {
        (void)note_list(*database_);
    });
    expect_note_error(NoteFailure::saving, "The change could not be saved.", [&] {
        (void)note_rename(*database_, 1, "Renamed");
    });
}

TEST_F(NoteStorageTest, DocumentsAndByteLimit) {
    const auto fixture = load_byte_fixture();
    for (const auto& [name, document] : fixture["samples"].items()) {
        SCOPED_TRACE(name);
        EXPECT_EQ(
            document.dump().size(),
            fixture["sample_bytes"][name].get<std::size_t>());
    }
    const auto limit = fixture["limit"].get<std::size_t>();
    EXPECT_EQ(limit, note_document_byte_limit);
    const auto empty_padding = padded_document(fixture["padding"]["document"], 0);
    const auto overhead = empty_padding.dump().size();
    const auto exact = padded_document(
        fixture["padding"]["document"], limit - overhead);
    const auto over = padded_document(
        fixture["padding"]["document"], limit - overhead + 1);
    EXPECT_EQ(exact.dump().size(), limit);
    EXPECT_EQ(over.dump().size(), limit + 1);

    const auto note = note_create(*database_, std::nullopt, NoteItemKind::note, "Body");
    const auto image = fixture["samples"]["image"];
    note_save(*database_, note.id, image);
    note_save(*database_, note.id, image);
    EXPECT_EQ(note_get(*database_, note.id).document, image);

    const auto opaque = nlohmann::json::parse(R"({"z":1,"a":[true],"hello":"foreign"})");
    note_save(*database_, note.id, opaque);
    EXPECT_EQ(note_get(*database_, note.id).document, opaque);

    note_save(*database_, note.id, exact);
    EXPECT_EQ(note_get(*database_, note.id).document, exact);
    expect_note_error(
        NoteFailure::too_large,
        "Note exceeds 12 MiB. Remove content or use a smaller picture.",
        [&] { note_save(*database_, note.id, over); });
    EXPECT_EQ(raw_content(*database_, note.id), exact.dump());

    expect_note_error(
        NoteFailure::invalid,
        "Note content must be a JSON object.",
        [&] { note_save(*database_, note.id, nlohmann::json::array()); });
    expect_note_error(
        NoteFailure::invalid,
        "That item is not a note.",
        [&] {
            const auto folder = note_create(
                *database_, std::nullopt, NoteItemKind::folder, "Folder");
            (void)note_get(*database_, folder.id);
        });

    auto write_content = [&](std::string content) {
        Statement update = database_->prepare(
            "UPDATE note_items SET content_json = ?1 WHERE id = ?2");
        update.bind_blob(1, compressed_content(content));
        update.bind(2, note.id);
        update.run();
    };
    write_content("{");
    const auto listed = note_list(*database_);
    EXPECT_FALSE(listed.empty());
    expect_note_error(NoteFailure::opening, "The note could not be opened.", [&] {
        (void)note_get(*database_, note.id);
    });
    expect_note_error(NoteFailure::opening, "The note could not be opened.", [&] {
        (void)note_get_text(*database_, note.id);
    });

    write_content("[]");
    expect_note_error(
        NoteFailure::opening,
        "The note could not be opened.",
        [&] { (void)note_get(*database_, note.id); });
    expect_note_error(
        NoteFailure::opening,
        "The note could not be opened.",
        [&] { (void)note_get_text(*database_, note.id); });

    write_content(over.dump());
    expect_note_error(
        NoteFailure::too_large,
        "Note exceeds 12 MiB. Remove content or use a smaller picture.",
        [&] { (void)note_get(*database_, note.id); });
    expect_note_error(
        NoteFailure::too_large,
        "Note exceeds 12 MiB. Remove content or use a smaller picture.",
        [&] { (void)note_get_text(*database_, note.id); });
    EXPECT_NO_THROW(validate_workspace_session_contents(*database_));
}

TEST_F(NoteStorageTest, FullCopyAndProtectionPreserveImageAndSequence) {
    const auto fixture = load_byte_fixture();
    const auto image = fixture["samples"]["image"];
    const auto note = note_create(*database_, std::nullopt, NoteItemKind::note, "Image");
    note_save(*database_, note.id, image);
    note_delete(*database_, note.id);
    const auto replacement = note_create(
        *database_, std::nullopt, NoteItemKind::note, "Image");
    note_save(*database_, replacement.id, image);
    EXPECT_GT(replacement.id, note.id);
    const auto sequence = note_sequence(*database_);
    const auto stored = raw_content(*database_, replacement.id);
    database_.reset();

    const auto copied = workspace_.root() / "copied.sqlite3";
    copy_workspace_session_database(path_, copied, {}, "secret");
    {
        Database protected_copy(copied, Database::Mode::read_write, "secret");
        EXPECT_EQ(protected_copy.pragma_integer("user_version"),
            workspace_session_database_version);
        EXPECT_EQ(protected_copy.pragma_integer("application_id"),
            workspace_session_application_id);
        EXPECT_NO_THROW(validate_workspace_session_contents(protected_copy));
        EXPECT_EQ(raw_content(protected_copy, replacement.id), stored);
        EXPECT_EQ(note_sequence(protected_copy), sequence);
        const auto loaded = note_get(protected_copy, replacement.id);
        EXPECT_EQ(loaded.document, image);
        const auto after_delete = note_create(
            protected_copy, std::nullopt, NoteItemKind::folder, "Later");
        EXPECT_GT(after_delete.id, replacement.id);
    }

    protect_workspace_session_database(path_, "secret");
    Database protected_original(path_, Database::Mode::read_write, "secret");
    EXPECT_EQ(raw_content(protected_original, replacement.id), stored);
    EXPECT_EQ(note_sequence(protected_original), sequence);
    EXPECT_NO_THROW(validate_workspace_session_contents(protected_original));
}

TEST_F(NoteStorageTest, AudioAttemptTransactionsAndExactIdentity) {
    create_event_history_schema(*database_);
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Chapter 1");
    (void)note_set_read_only(*database_, note.id, true);
    EXPECT_THROW(note_audio_accept(*database_, note.id, 1, "https://audio.test", "key-1",
        [](const auto&) { throw std::runtime_error("launch failed"); }), std::runtime_error);
    EXPECT_FALSE(note_audio_attempt_for_note(*database_, note.id));
    const auto attempt = *note_audio_accept(*database_, note.id, 1, "https://audio.test", "key-1", [](const auto&) {});
    EXPECT_FALSE(attempt.batch_id);
    EXPECT_THROW(note_audio_accept(*database_, note.id, 1, "https://audio.test", "key-1", [](const auto&) {}), NoteError);
    (void)note_rename(*database_, note.id, "New name");
    EXPECT_TRUE(note_audio_register(*database_, attempt.id, "batches/example", "Kore"));
    EXPECT_TRUE(note_audio_register(*database_, attempt.id, "batches/example", "Kore"));
    ASSERT_EQ(event_list(*database_).size(), 1u);
    EXPECT_NE(event_list(*database_)[0].message.find("Chapter 1"), std::string::npos);
    const auto clip = workspace_.root() / "clip.m4a";
    { std::ofstream output(clip); output << "audio"; }
    note_save_audio(*database_, note.id, {clip});
    { std::ofstream output(clip); output << "replacement"; }
    database_->execute("CREATE TRIGGER fail_event BEFORE INSERT ON event_history BEGIN SELECT RAISE(ABORT,'disk'); END");
    EXPECT_THROW(note_audio_finish(*database_, attempt.id, {clip}), std::runtime_error);
    EXPECT_EQ(note_audio_clip(*database_, note.id, 0).bytes, "audio");
    EXPECT_TRUE(note_audio_attempt_for_note(*database_, note.id));
    EXPECT_THROW((void)note_set_read_only(*database_, note.id, false), NoteError);
    EXPECT_TRUE(note_audio_attempt_for_note(*database_, note.id));
    database_->execute("DROP TRIGGER fail_event");
    EXPECT_TRUE(note_audio_finish(*database_, attempt.id, {clip}));
    EXPECT_EQ(note_audio_clip(*database_, note.id, 0).bytes, "replacement");
    EXPECT_FALSE(note_audio_attempt_for_note(*database_, note.id));
    const auto next = *note_audio_accept(*database_, note.id, 1, "https://audio.test", "key-1", [](const auto&) {});
    EXPECT_GT(next.id, attempt.id);
    EXPECT_FALSE(note_audio_register(*database_, attempt.id, "batches/late", "Kore"));
    EXPECT_FALSE(note_audio_retire(*database_, attempt.id, EventSeverity::error, "late"));
    EXPECT_FALSE(note_audio_finish(*database_, attempt.id, {clip}));
    EXPECT_FALSE(note_audio_attempt_by_id(*database_, next.id)->batch_id);
    (void)note_set_read_only(*database_, note.id, false);
    EXPECT_FALSE(note_audio_attempt_for_note(*database_, note.id));
    EXPECT_EQ(event_list(*database_)[0].severity, EventSeverity::warning);
}

TEST_F(NoteStorageTest, AudioAttemptDeletionAbandonsDescendantsAtomically) {
    create_event_history_schema(*database_);
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Book");
    const auto note = note_create(*database_, folder.id, NoteItemKind::note, "Chapter");
    (void)note_set_read_only(*database_, note.id, true);
    const auto attempt = *note_audio_accept(*database_, note.id, 1, "https://audio.test", "key-1", [](const auto&) {});
    database_->execute("CREATE TRIGGER fail_delete BEFORE DELETE ON note_items BEGIN SELECT RAISE(ABORT,'disk'); END");
    EXPECT_THROW(note_delete(*database_, folder.id), NoteError);
    ASSERT_TRUE(note_audio_attempt_for_note(*database_, note.id));
    EXPECT_EQ(note_audio_attempt_for_note(*database_, note.id)->id, attempt.id);
    EXPECT_TRUE(event_list(*database_).empty());
    database_->execute("DROP TRIGGER fail_delete");
    note_delete(*database_, folder.id);
    EXPECT_FALSE(note_audio_attempt_for_note(*database_, note.id));
    EXPECT_EQ(event_list(*database_).size(), 1u);
}

TEST_F(NoteStorageTest, IndependentAudioAttemptsUseExactIdentity) {
    create_event_history_schema(*database_);
    const auto a = note_create(*database_, {}, NoteItemKind::note, "A");
    const auto b = note_create(*database_, {}, NoteItemKind::note, "B");
    (void)note_set_read_only(*database_, a.id, true);
    (void)note_set_read_only(*database_, b.id, true);
    const auto first = *note_audio_accept(*database_, a.id, 1, "https://audio.test", "key", [](const auto&) {});
    const auto second = *note_audio_accept(*database_, b.id, 1, "https://audio.test", "key", [](const auto&) {});
    ASSERT_EQ(note_audio_attempts(*database_).size(), 2u);
    EXPECT_THROW(note_audio_accept(*database_, a.id, 1, "https://audio.test", "key", [](const auto&) {}), NoteError);
    EXPECT_TRUE(note_audio_register(*database_, second.id, "batches/b", "Kore"));
    EXPECT_FALSE(note_audio_attempt_by_id(*database_, first.id)->batch_id);
    EXPECT_TRUE(note_audio_retire(*database_, second.id, EventSeverity::error, "B failed"));
    EXPECT_TRUE(note_audio_attempt_for_note(*database_, a.id));
    EXPECT_FALSE(note_audio_attempt_for_note(*database_, b.id));
}

TEST_F(NoteStorageTest, MissingOnlyAcceptanceAndRollbackKeepOtherAttemptsAndAudio) {
    create_event_history_schema(*database_);
    const auto a = note_create(*database_, {}, NoteItemKind::note, "A");
    const auto b = note_create(*database_, {}, NoteItemKind::note, "B");
    const auto c = note_create(*database_, {}, NoteItemKind::note, "C");
    for (auto id : {a.id, b.id, c.id}) (void)note_set_read_only(*database_, id, true);
    int launches = 0;
    const auto launch = [&](const auto&) { ++launches; };
    const auto first = *note_audio_accept(*database_, a.id, 1, "https://audio.test", "key", launch);
    EXPECT_FALSE(note_audio_accept(*database_, a.id, 1, "https://audio.test", "key", launch, true));
    const auto clip = workspace_.root() / "clip.m4a";
    { std::ofstream output(clip); output << "existing"; }
    note_save_audio(*database_, b.id, {clip}); // Audio appeared after the UI scan.
    EXPECT_FALSE(note_audio_accept(*database_, b.id, 1, "https://audio.test", "key", launch, true));
    EXPECT_EQ(launches, 1);
    EXPECT_EQ(note_audio_clip(*database_, b.id, 0).bytes, "existing");
    EXPECT_THROW(note_audio_accept(*database_, c.id, 1, "https://audio.test", "key",
        [](const auto&) { throw std::runtime_error("launch"); }), std::runtime_error);
    EXPECT_EQ(note_audio_attempts(*database_).size(), 1u);
    const auto second = *note_audio_accept(*database_, b.id, 1, "https://audio.test", "key", launch);
    EXPECT_TRUE(note_audio_register(*database_, second.id, "batches/b", "Kore"));
    EXPECT_TRUE(note_audio_finish(*database_, second.id, {clip}));
    EXPECT_TRUE(note_audio_attempt_by_id(*database_, first.id));
    EXPECT_EQ(event_list(*database_).size(), 2u);
    (void)note_set_read_only(*database_, c.id, false);
    EXPECT_FALSE(note_audio_accept(*database_, c.id, 1, "https://audio.test", "key", launch, true));
    EXPECT_FALSE(note_audio_accept(*database_, 9999, 1, "https://audio.test", "key", launch, true));
    (void)note_set_read_only(*database_, a.id, false);
    EXPECT_FALSE(note_audio_attempt_by_id(*database_, first.id));
}

TEST_F(NoteStorageTest, AddingPerNoteIndexPreservesRegisteredAttempt) {
    create_event_history_schema(*database_);
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Saved");
    (void)note_set_read_only(*database_, note.id, true);
    const auto attempt = *note_audio_accept(*database_, note.id, 1, "https://audio.test", "key", [](const auto&) {});
    ASSERT_TRUE(note_audio_register(*database_, attempt.id, "batches/saved", "Kore"));
    database_->execute("DROP INDEX note_audio_attempts_note");
    create_note_items_schema(*database_);
    const auto saved = note_audio_attempt_by_id(*database_, attempt.id);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->batch_id, "batches/saved");
    EXPECT_EQ(saved->note_id, note.id);
}

TEST_F(NoteStorageTest, EventTrimmingKeepsEveryPendingAudioAttempt) {
    create_event_history_schema(*database_);
    std::vector<std::int64_t> attempts;
    for (const auto* name : {"A", "B"}) {
        const auto note = note_create(*database_, {}, NoteItemKind::note, name);
        (void)note_set_read_only(*database_, note.id, true);
        const auto attempt = *note_audio_accept(*database_, note.id, 1, "https://audio.test", "key", [](const auto&) {});
        ASSERT_TRUE(note_audio_register(*database_, attempt.id, std::string("batches/") + name, "Kore"));
        attempts.push_back(attempt.id);
    }
    for (int i = 0; i < event_history_limit; ++i) event_append(*database_, EventSeverity::info, "Later event");
    const auto events = event_list(*database_);
    ASSERT_EQ(events.size(), event_history_limit);
    EXPECT_EQ(events.back().message, "Later event");
    for (const auto id : attempts) {
        const auto attempt = note_audio_attempt_by_id(*database_, id);
        ASSERT_TRUE(attempt);
        EXPECT_TRUE(attempt->batch_id);
    }
}

TEST_F(NoteStorageTest, DeletingFolderAbandonsAllItsAudioAttempts) {
    create_event_history_schema(*database_);
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Book");
    const auto nested = note_create(*database_, folder.id, NoteItemKind::folder, "Part");
    const auto a = note_create(*database_, folder.id, NoteItemKind::note, "A");
    const auto b = note_create(*database_, nested.id, NoteItemKind::note, "B");
    const auto outside = note_create(*database_, {}, NoteItemKind::note, "Outside");
    for (const auto id : {a.id, b.id, outside.id}) {
        (void)note_set_read_only(*database_, id, true);
        (void)note_audio_accept(*database_, id, 1, "https://audio.test", "key", [](const auto&) {});
    }
    database_->execute("CREATE TRIGGER fail_delete BEFORE DELETE ON note_items BEGIN SELECT RAISE(ABORT,'disk'); END");
    EXPECT_THROW(note_delete(*database_, folder.id), NoteError);
    EXPECT_EQ(note_audio_attempts(*database_).size(), 3u);
    EXPECT_TRUE(event_list(*database_).empty());
    database_->execute("DROP TRIGGER fail_delete");
    note_delete(*database_, folder.id);
    ASSERT_EQ(note_audio_attempts(*database_).size(), 1u);
    EXPECT_EQ(note_audio_attempts(*database_).front().note_id, outside.id);
    EXPECT_EQ(event_list(*database_).size(), 2u);
}

TEST(NoteStorage, OlderVaultAndMaintenanceKeepIdentityWithoutConfigChanges) {
    test::TestWorkspace workspace;
    const auto path = test::import_test_database(workspace.root());
    std::int64_t application_id = 0;
    {
        Database database(path, Database::Mode::read_write);
        database.execute("DROP TABLE session_notes");
        database.execute("DROP TABLE session_files");
        database.execute("DROP TABLE note_items");
        application_id = database.pragma_integer("application_id");
        EXPECT_EQ(
            database.pragma_integer("user_version"),
            workspace_session_database_version);
    }

    auto store = WorkspaceConfigStore::open(path);
    EXPECT_EQ(store->list_note_items().size(), 2u);
    const auto revision = store->config_revision();
    const auto snapshot = store->snapshot();
    const auto folder = store->create_note_item(
        std::nullopt, NoteItemKind::folder, "Plan");
    const auto note = store->create_note_item(std::nullopt, NoteItemKind::note, "Draft");
    store->save_note(note.id, nlohmann::json::parse(R"({"type":"doc","extra":1})"));
    (void)store->rename_note_item(note.id, "Final");
    store->delete_note_item(folder.id);
    EXPECT_EQ(store->config_revision(), revision);
    EXPECT_EQ(store->snapshot().get(), snapshot.get());
    EXPECT_EQ(store->list_note_items().size(), 3u);

    {
        auto maintenance = store->reserve_maintenance();
        maintenance.close();
        {
            Database database(path, Database::Mode::read_write);
            database.execute("DROP TABLE note_items");
        }
        maintenance.reopen();
    }
    EXPECT_EQ(store->list_note_items().size(), 2u);
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_EQ(database.pragma_integer("application_id"), application_id);
        EXPECT_EQ(
            database.pragma_integer("user_version"),
            workspace_session_database_version);
        EXPECT_TRUE(has_schema_object(database, "table", "note_items"));
        EXPECT_TRUE(has_schema_object(database, "table", "session_notes"));
        EXPECT_TRUE(has_schema_object(database, "table", "session_files"));
        EXPECT_NO_THROW(validate_workspace_session_contents(database));
    }
}

} // namespace
} // namespace cha
