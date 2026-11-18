#include "notes/notes_service.h"
#include "app/background_jobs.h"
#include "providers/api_key_store.h"
#include "providers/model_backend.h"
#include "providers/openai_oauth.h"
#include "runtime/runtime_settings.h"
#include "runtime/live_session_manager.h"
#include "storage/session_repository.h"
#include "storage/sqlite_storage.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"
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

TEST_F(NotesServiceTest, SummaryBatchRejectsIncompatibleEffortBeforeRecordingAnEvent) {
    const auto note = notes.create_note_item({}, NoteItemKind::note, "Chapter", 1);
    notes.save_note(note.id, text_document(std::string(4096, 'x')), 1);
    const auto provider_id = store->create_provider("Incompatible", "test");
    auto config = store->snapshot()->find_provider(provider_id)->config;
    config.mode = Mode::net;
    config.api = ProviderApi::responses;
    config.host = "api.openai.com";
    config.model = "gpt-4o";
    store->apply_provider_update(provider_id, "Incompatible", config);
    try {
        (void)notes.start_summary_batch("owner", note.id, {provider_id, "high", {"l1"}, true}, 1);
        FAIL() << "Expected the requested effort to be rejected";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::invalid_argument);
        EXPECT_STREQ(error.what(), "This compression provider's model does not support high reasoning effort.");
    }
    EXPECT_TRUE(store->list_events().empty());
    EXPECT_NO_THROW((void)notes.start_summary_batch("owner", note.id, {"test", "none", {"l1"}, true}, 1));
    notes.finish_summary_batch("owner", 1);
}

TEST_F(NotesServiceTest, SummaryBatchRecordsOneTerminalEventAndFreesTheSlot) {
    const auto note = notes.create_note_item({}, NoteItemKind::note, "Chapter", 1);
    notes.save_note(note.id, text_document(std::string(4096, 'x')), 1);
    const auto label = "“Chapter” (note " + std::to_string(note.id) + ")";
    EXPECT_EQ(notes.start_summary_batch("owner", note.id, {"test", "none", {"l1"}, true}, 1)["notes"].size(), 1U);
    notes.rename_note_item(note.id, "Renamed", 1);
    EXPECT_THROW(notes.start_summary_batch("other", note.id, {"test", "none", {"l1"}, true}, 1), ApplicationError);
    notes.finish_summary_batch("other", 1);
    {
        const std::lock_guard lock(lifecycle);
        notes.cancel_summaries_locked("other");
        EXPECT_EQ(store->list_events().size(), 1U);
        notes.cancel_summaries_locked("owner");
        notes.cancel_summaries_locked();
    }
    const auto cancelled = store->list_events();
    ASSERT_EQ(cancelled.size(), 2U);
    EXPECT_EQ(cancelled[1].message, "Summarization started for " + label + ", 1 note.");
    EXPECT_EQ(cancelled[0].message, "Summarization cancelled for " + label + ".");
    EXPECT_EQ(cancelled[0].severity, EventSeverity::info);
    (void)notes.start_summary_batch("other", note.id, {"test", "none", {"l1"}, true}, 1);
    notes.finish_summary_batch("other", 1);
    notes.finish_summary_batch("other", 1);
    const auto finished = store->list_events();
    ASSERT_EQ(finished.size(), 4U);
    EXPECT_EQ(finished.front().message, "Summarization finished for “Renamed” (note " + std::to_string(note.id) + ").");
}

TEST_F(NotesServiceTest, SummaryBatchRequiresTheStartEventAndFreesTheSlotIfTheEndEventFails) {
    const auto note = notes.create_note_item({}, NoteItemKind::note, "Chapter", 1);
    notes.save_note(note.id, text_document(std::string(4096, 'x')), 1);
    storage::SqliteDatabase database(store->database_path(), storage::SqliteDatabase::Mode::read_write);
    database.execute("ALTER TABLE event_history RENAME TO unavailable_events");
    EXPECT_THROW(notes.start_summary_batch("first", note.id, {"test", "none", {"l1"}, true}, 1), std::runtime_error);
    database.execute("ALTER TABLE unavailable_events RENAME TO event_history");
    EXPECT_TRUE(store->list_events().empty());
    EXPECT_NO_THROW((void)notes.start_summary_batch("first", note.id, {"test", "none", {"l1"}, true}, 1));
    database.execute("ALTER TABLE event_history RENAME TO unavailable_events");
    EXPECT_NO_THROW(notes.finish_summary_batch("first", 1));
    database.execute("ALTER TABLE unavailable_events RENAME TO event_history");
    EXPECT_NO_THROW((void)notes.start_summary_batch("second", note.id, {"test", "none", {"l1"}, true}, 1));
    notes.finish_summary_batch("second", 1);
    EXPECT_EQ(store->list_events().size(), 3U);
}

TEST(FlexTest, MapsTheResultToItsPanelText) {
    GenerationResult result;
    EXPECT_EQ(flex_test_failure(result), std::nullopt);
    result.service_tier = "flex";
    EXPECT_EQ(flex_test_failure(result), std::nullopt);
    result.service_tier = "default";
    EXPECT_EQ(flex_test_failure(result), "The provider did not use Flex for this request.");
    result.outcome = GenerationOutcome::protocol_error;
    EXPECT_EQ(flex_test_failure(result), "Flex test failed: The provider returned an error.");
    result.message = "Provider rate limit exceeded.";
    EXPECT_EQ(flex_test_failure(result), "Flex test failed: Provider rate limit exceeded.");
}

} // namespace
} // namespace cha::app
