#include "notes/notes_service.h"
#include "app/background_jobs.h"
#include "providers/api_key_store.h"
#include "providers/openai_oauth.h"
#include "runtime/runtime_settings.h"
#include "runtime/live_session_manager.h"
#include "storage/session_repository.h"
#include "workspace/builtins.h"
#include "support/test_workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <algorithm>

namespace cha::app {
namespace {

class NotesServiceTest : public testing::Test {
protected:
    test::TestWorkspace workspace;
    std::unique_ptr<WorkspaceConfigStore> store =
        WorkspaceConfigStore::open(test::import_test_database(workspace.root()));
    std::timed_mutex lifecycle;
    std::optional<ErrorCode> denied;
    ApiKeyStore keys{*store, workspace.root() / "api-keys.json"};
    OpenAiOAuth oauth{workspace.root() / "openai-auth.json"};
    BackgroundJobs jobs;
    RuntimeSettings settings;
    std::atomic_uint64_t epoch{1};
    SessionRepository sessions{[this] { return store->snapshot(); }, store->database_path(),
        store->workspace_path(), store->welcome_path(),
        {{std::string(entrance_id), std::string(welcome_id)}, std::string(welcome_name)}};
    LiveSessionManager live_sessions{settings,
        [](const FullSessionId&, std::shared_ptr<WakeNotifier>, std::uint64_t) -> OpenedSession {
            throw std::logic_error("These service tests do not open live sessions");
        }};
    void TearDown() override { jobs.request_stop(); jobs.join(); }
    NotesService notes{*store, keys, oauth, sessions, live_sessions, jobs, settings, lifecycle, epoch, [this](std::uint64_t epoch) {
        return epoch == 1 ? denied : std::optional{ErrorCode::vault_changed};
    }, [] { return true; }, true, [] { return std::string("Test"); }};
};

nlohmann::json text_document(std::string text) {
    return {{"type", "doc"}, {"content", nlohmann::json::array({
        {{"type", "paragraph"}, {"content", nlohmann::json::array({
            {{"type", "text"}, {"text", std::move(text)}}})}}})}};
}

TEST_F(NotesServiceTest, SavesLoadsRenamesAndKeepsScrollPosition) {
    const auto item = notes.create_note_item({}, NoteItemKind::note, "Draft", 1);
    const auto document = text_document("Saved text");
    EXPECT_FALSE(notes.save_note(item.id, document, 1)); // No compressed text was invalidated.
    notes.save_note_scroll_position(item.id, 37, 1);
    const auto saved = notes.get_note(item.id, 1);
    EXPECT_EQ(saved.document, document);
    EXPECT_EQ(saved.scroll_top, 37);
    EXPECT_EQ(notes.rename_note_item(item.id, "Renamed", 1).name, "Renamed");
    const auto items = notes.list_note_items(1);
    const auto found = std::find_if(items.begin(), items.end(), [&](const auto& row) { return row.id == item.id; });
    ASSERT_NE(found, items.end());
    EXPECT_EQ(found->name, "Renamed");
}

TEST_F(NotesServiceTest, ImportsIntoTheSelectedFolderAndDeletesOnlyThatSubtree) {
    const auto folder = notes.create_note_item({}, NoteItemKind::folder, "Books", 1);
    const auto other = notes.create_note_item({}, NoteItemKind::note, "Keep", 1);
    const auto document = text_document("Chapter text");
    const auto book = notes.import_note_book(folder.id, "Book", {{"Chapter", document}}, 1);
    const auto items = notes.list_note_items(1);
    const auto chapter = std::find_if(items.begin(), items.end(), [&](const auto& row) { return row.parent_id == book; });
    ASSERT_NE(chapter, items.end());
    EXPECT_EQ(notes.get_note(chapter->id, 1).document, document);
    notes.delete_note_item(folder.id, 1);
    EXPECT_NO_THROW((void)notes.get_note(other.id, 1));
    try { (void)notes.get_note(chapter->id, 1); FAIL(); }
    catch (const ApplicationError& error) { EXPECT_EQ(error.code, ErrorCode::not_found); }
}

TEST_F(NotesServiceTest, TranslatesMissingAndOversizedDocumentErrors) {
    try { (void)notes.get_note(note_id_limit, 1); FAIL(); }
    catch (const ApplicationError& error) { EXPECT_EQ(error.code, ErrorCode::not_found); }
    const auto item = notes.create_note_item({}, NoteItemKind::note, "Draft", 1);
    try { (void)notes.save_note(item.id, text_document(std::string(note_document_byte_limit, 'x')), 1); FAIL(); }
    catch (const ApplicationError& error) { EXPECT_EQ(error.code, ErrorCode::body_too_large); }
    EXPECT_EQ(notes.get_note(item.id, 1).document, nlohmann::json::parse(empty_note_document));
}

TEST_F(NotesServiceTest, EditingRespectsReadOnlyNotes) {
    const auto item = notes.create_note_item({}, NoteItemKind::note, "Draft", 1);
    (void)notes.set_note_read_only(item.id, true, 1);
    try { (void)notes.save_note(item.id, text_document("Cannot save"), 1); FAIL(); }
    catch (const ApplicationError& error) { EXPECT_EQ(error.code, ErrorCode::invalid_argument); }
}

TEST_F(NotesServiceTest, AdmissionRejectsBeforeMutation) {
    const auto item = notes.create_note_item({}, NoteItemKind::note, "Draft", 1);
    denied = ErrorCode::application_unavailable;
    try { (void)notes.rename_note_item(item.id, "Changed", 1); FAIL(); }
    catch (const ApplicationError& error) { EXPECT_EQ(error.code, *denied); }
    denied.reset();
    try { (void)notes.get_note(item.id, 2); FAIL(); }
    catch (const ApplicationError& error) { EXPECT_EQ(error.code, ErrorCode::vault_changed); }
    EXPECT_EQ(store->list_note_items().back().name, "Draft");
}

} // namespace
} // namespace cha::app
