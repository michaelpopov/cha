#include "storage/note_storage.h"
#include "storage/session_label.h"
#include "storage/sqlite_storage.h"
#include "storage/workspace_session_database.h"
#include "support/test_workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <string_view>

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

std::string raw_content(Database& database, std::int64_t id) {
    Statement statement = database.prepare(
        "SELECT content_json FROM note_items WHERE id = ?1");
    statement.bind(1, id);
    EXPECT_TRUE(statement.step());
    EXPECT_FALSE(statement.is_null(0));
    return statement.text(0);
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
    spaced.bind(1, spaced_json);
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

TEST_F(NoteStorageTest, DeleteCascadesAndDoesNotReuseIds) {
    const auto root = note_create(*database_, std::nullopt, NoteItemKind::folder, "Root");
    const auto child = note_create(*database_, root.id, NoteItemKind::folder, "Child");
    const auto grandchild = note_create(
        *database_, child.id, NoteItemKind::note, "Grandchild");
    const auto sibling = note_create(
        *database_, std::nullopt, NoteItemKind::note, "Sibling");
    note_delete(*database_, root.id);
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
        update.bind(1, content);
        update.bind(2, note.id);
        update.run();
    };
    write_content("{");
    const auto listed = note_list(*database_);
    EXPECT_FALSE(listed.empty());
    expect_note_error(NoteFailure::opening, "The note could not be opened.", [&] {
        (void)note_get(*database_, note.id);
    });

    write_content("[]");
    expect_note_error(
        NoteFailure::opening,
        "The note could not be opened.",
        [&] { (void)note_get(*database_, note.id); });

    write_content(over.dump());
    expect_note_error(
        NoteFailure::too_large,
        "Note exceeds 12 MiB. Remove content or use a smaller picture.",
        [&] { (void)note_get(*database_, note.id); });
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

TEST(NoteStorage, OlderVaultAndMaintenanceKeepIdentityWithoutConfigChanges) {
    test::TestWorkspace workspace;
    const auto path = test::import_test_database(workspace.root());
    std::int64_t application_id = 0;
    {
        Database database(path, Database::Mode::read_write);
        database.execute("DROP TABLE note_items");
        application_id = database.pragma_integer("application_id");
        EXPECT_EQ(
            database.pragma_integer("user_version"),
            workspace_session_database_version);
    }

    auto store = WorkspaceConfigStore::open(path);
    EXPECT_TRUE(store->list_note_items().empty());
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
    EXPECT_EQ(store->list_note_items().size(), 1U);

    {
        auto maintenance = store->reserve_maintenance();
        maintenance.close();
        {
            Database database(path, Database::Mode::read_write);
            database.execute("DROP TABLE note_items");
        }
        maintenance.reopen();
    }
    EXPECT_TRUE(store->list_note_items().empty());
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_EQ(database.pragma_integer("application_id"), application_id);
        EXPECT_EQ(
            database.pragma_integer("user_version"),
            workspace_session_database_version);
        EXPECT_TRUE(has_schema_object(database, "table", "note_items"));
        EXPECT_NO_THROW(validate_workspace_session_contents(database));
    }
}

} // namespace
} // namespace cha
