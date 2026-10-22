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

TEST_F(NoteStorageTest, AddsCompressionColumnToExistingNotesOnce) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Old note");
    database_->execute("ALTER TABLE note_items DROP COLUMN compressed_text");
    for (int count = 0; count < 2; ++count) {
        storage::SqliteTransaction transaction(*database_);
        create_note_items_schema(*database_);
        transaction.commit();
    }
    EXPECT_FALSE(note_list(*database_).front().has_compressed);
    EXPECT_EQ(note_get(*database_, note.id).document, nlohmann::json::parse(empty_note_document));
    note_save_compressed(*database_, note.id, "Compressed after upgrade");
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

TEST_F(NoteStorageTest, AddsReadOnlyColumnToExistingNotesOnceAndKeepsThemEditable) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Existing");
    database_->execute("ALTER TABLE note_items DROP COLUMN read_only");
    for (int count = 0; count < 2; ++count) {
        storage::SqliteTransaction transaction(*database_);
        create_note_items_schema(*database_);
        transaction.commit();
    }
    EXPECT_FALSE(note_list(*database_).front().read_only);
    note_save(*database_, note.id, {{"changed", true}});
    EXPECT_TRUE(note_set_read_only(*database_, note.id, true).read_only);
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

TEST_F(NoteStorageTest, ReadOnlyNotesSaveNewBlueCommentsButKeepExistingContent) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Reading");
    const auto original = nlohmann::json::parse(R"({"type":"doc","content":[
        {"type":"paragraph","content":[{"type":"text","text":"Alpha"}]},
        {"type":"bulletList","content":[{"type":"listItem","content":[
            {"type":"paragraph","content":[{"type":"text","text":"Nested"}]}
        ]}]},
        {"type":"paragraph","content":[{"type":"text","text":"Original blue text",
            "marks":[{"type":"highlight","attrs":{"color":"#b3d9ff"}}]}]}
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
    changed["content"].erase(3); // Original blue text is protected too.
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    changed = commented;
    changed["content"][0]["content"][0]["text"] = "Changed comment";
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    changed = commented;
    auto unhighlighted = comment;
    unhighlighted["content"][0]["marks"][0]["attrs"]["color"] = "yellow";
    unhighlighted["content"][0]["text"] = "Different unhighlighted text";
    auto& changed_blocks = changed["content"].get_ref<nlohmann::json::array_t&>();
    changed_blocks.insert(changed_blocks.begin(), unhighlighted);
    EXPECT_THROW(note_save(*database_, note.id, changed), NoteError);
    EXPECT_EQ(note_get(*database_, note.id).document, commented);

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

TEST_F(NoteStorageTest, AddsScrollColumnToExistingNotesOnceAndPreservesContent) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Existing");
    const auto original = raw_content(*database_, note.id);
    database_->execute("ALTER TABLE note_items DROP COLUMN scroll_top");
    for (int count = 0; count < 2; ++count) {
        storage::SqliteTransaction transaction(*database_);
        create_note_items_schema(*database_);
        transaction.commit();
    }
    EXPECT_EQ(note_get(*database_, note.id).scroll_top, 0);
    EXPECT_EQ(raw_content(*database_, note.id), original);
    note_save_scroll_position(*database_, note.id, 640);
    EXPECT_EQ(note_get(*database_, note.id).scroll_top, 640);
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
        EXPECT_TRUE(has_schema_object(database, "table", "session_notes"));
        EXPECT_TRUE(has_schema_object(database, "table", "session_files"));
        EXPECT_NO_THROW(validate_workspace_session_contents(database));
    }
}

} // namespace
} // namespace cha
