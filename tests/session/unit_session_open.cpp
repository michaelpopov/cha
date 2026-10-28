#include "session/session_open.h"

#include "providers/providers.h"
#include "providers/api_key_store.h"
#include "runtime/text_input.h"
#include "session/session_controller.h"
#include "storage/folder_store_storage.h"
#include "storage/not_found_error.h"
#include "storage/session_repository.h"
#include "storage/note_storage.h"
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
            [this] { return store_->snapshot(); },
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
    ASSERT_NE(store_->snapshot()->find_forum("lobby"), nullptr);
}

TEST_F(SessionOpenTest, OpensWelcomeThroughTheSamePath) {
    OpenedSession opened = open(
        {std::string(entrance_id), std::string(welcome_id)});

    EXPECT_EQ(opened.label, welcome_name);
    EXPECT_EQ(
        opened.controller->view().default_persona_id,
        workspace_guest_id);
}

TEST_F(SessionOpenTest, WelcomeCopiesNotesFromTheActiveVaultAndKeepsItsOwnCopy) {
    const auto note = store_->create_note_item(std::nullopt, NoteItemKind::note, "Reference");
    store_->save_note(note.id, nlohmann::json::parse(
        R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Original"}]}]})"));
    const FullSessionId welcome{std::string(entrance_id), std::string(welcome_id)};
    {
        auto opened = open(welcome);
        ASSERT_TRUE(opened.controller->add_note(note.id).input_consumed);
        ASSERT_EQ(opened.controller->view().transcript.entries.size(), 1U);
        EXPECT_EQ(opened.controller->view().transcript.entries.front().text,
            "Added note “Reference” to this chat.");
    }
    const auto prepared = sessions_->prepare(welcome);
    SessionJournal journal(prepared.database_path, prepared.session_key);
    EXPECT_EQ(journal.notes_prompt(), "# Note: Reference\n\nOriginal");
    store_->save_note(note.id, nlohmann::json::parse(
        R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Revised"}]}]})"));
    store_->save_compressed_note(note.id, "Compressed");
    EXPECT_EQ(store_->get_note_text(note.id).text, "Revised");
    {
        auto opened = open(welcome);
        EXPECT_EQ(journal.notes_prompt(), "# Note: Reference\n\nOriginal");
        ASSERT_TRUE(opened.controller->add_note(note.id).input_consumed);
        EXPECT_EQ(opened.controller->view().transcript.entries.back().text,
            "Updated this chat’s copy of note “Reference”.");
    }
    EXPECT_EQ(journal.notes_prompt(), "# Note: Reference\n\nCompressed");
    const auto other = sessions_->create("lobby", "Other");
    const auto other_prepared = sessions_->prepare(other.identity);
    SessionJournal vault_session(other_prepared.database_path, other_prepared.session_key);
    EXPECT_TRUE(vault_session.notes_prompt().empty());
}

TEST_F(SessionOpenTest, DefaultCharacterWriteSurvivesRestartAndExport) {
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
    }

    const std::shared_ptr<const Workspace> workspace = store_->snapshot();
    ASSERT_NE(workspace->find_forum("lobby"), nullptr);
    EXPECT_EQ(workspace->find_forum("lobby")->default_character_id, "writer");

    const std::string forum = config_content(
        store_->database_path(), "forums/lobby/config.toml");
    EXPECT_NE(forum.find("writer"), std::string::npos);

    sessions_.reset();
    store_.reset();
    open_runtime();
    ASSERT_NE(store_->snapshot()->find_forum("lobby"), nullptr);
    EXPECT_EQ(store_->snapshot()->find_forum("lobby")->default_character_id, "writer");
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
}

TEST_F(SessionOpenTest, SourceDirectoryEditsDoNotAffectOpening) {
    const StoredSession first = sessions_->create("lobby", "First");
    fixture_.write_character_config(
        "display_name = \"Renamed\"\nprovider = \"test\"\n");

    OpenedSession unchanged = open(first.identity);
    ASSERT_NE(store_->snapshot()->find_character("guide"), nullptr);
    EXPECT_EQ(
        store_->snapshot()->find_character("guide")->character.display_name,
        "Guide");

    unchanged.controller.reset();
    sessions_.reset();
    store_.reset();
    reimport_fixture();
    const StoredSession second = sessions_->create("lobby", "Second");
    OpenedSession reloaded = open(second.identity);
    ASSERT_NE(store_->snapshot()->find_character("guide"), nullptr);
    EXPECT_EQ(
        store_->snapshot()->find_character("guide")->character.display_name,
        "Renamed");
}

TEST_F(SessionOpenTest, SecondaryDefaultCharacterSelectionDoesNotPersistSettings) {
    auto secondary = WorkspaceConfigStore::open(database_);
    SessionRepository sessions([&] { return secondary->snapshot(); },
        secondary->database_path(), secondary->workspace_path(), secondary->welcome_path(),
        TemporarySessionSeed{{std::string(entrance_id), std::string(welcome_id)}, "Welcome"});
    const auto stored = sessions.create("lobby", "Secondary");
    auto opened = open_session(sessions, stored.identity, providers_,
        std::make_shared<test::TestNotifier>(), *secondary);
    const auto before = config_content(database_, "forums/lobby/config.toml");
    const auto snapshot = secondary->snapshot();
    EXPECT_NO_THROW(opened.persist_default_character("guide"));
    EXPECT_EQ(config_content(database_, "forums/lobby/config.toml"), before);
    EXPECT_EQ(secondary->snapshot(), snapshot);
    EXPECT_TRUE(opened.controller->view().transcript.entries.empty());
}

TEST_F(SessionOpenTest, ReportsMissingForumAndSession) {
    EXPECT_THROW(
        (void)open({"missing", "session"}),
        ForumNotFoundError);
    EXPECT_THROW(
        (void)open({"lobby", "missing"}),
        SessionNotFoundError);
}


class FolderRequestBackend final : public ModelBackend {
public:
    FolderRequestBackend(std::vector<GenerationRequest>& requests, bool& reject)
        : requests_(requests), reject_(reject) {}
    RequestPayload prepare(const GenerationRequest& input) override {
        requests_.push_back(input);
        return {};
    }
    GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink, const std::atomic_bool&) override {
        if (reject_ && requests_.back().folder_search && requests_.back().run.target.id == "guide")
            return {GenerationOutcome::protocol_error, "Provider rejected store count"};
        sink({GenerationDeltaKind::answer, "Answer"});
        return {};
    }
private:
    std::vector<GenerationRequest>& requests_;
    bool& reject_;
};

TEST_F(SessionOpenTest, FolderAccessResolvesPerTargetAndRefreshesAfterReplacementAndDetach) {
    fixture_.add_character("writer", "Writer");
    const auto member = fixture_.root() / "forums/lobby/members/writer";
    std::filesystem::create_directories(member);
    std::ofstream(member / "character.toml") << "# member\n";
    reimport_fixture();
    ApiKeyStore keys(*store_);
    const auto openai = keys.create("OpenAI", "same-secret");
    const auto google = keys.create("Google", "same-secret");
    ModelBackendConfig config;
    config.host = "api.openai.com"; config.port = 443; config.https = true;
    config.mode = Mode::net; config.api = ProviderApi::responses; config.model = "test";
    config.api_key_id = openai.id;
    (void)store_->apply_provider_update("test", "Test", config);
    const auto google_provider = store_->create_provider("Google", "test");
    config.host = "generativelanguage.googleapis.com"; config.api = ProviderApi::google;
    config.base_path = "/v1beta"; config.api_key_id = google.id;
    (void)store_->apply_provider_update(google_provider, "Google", config);
    (void)store_->apply_character_settings("writer", google_provider, {});
    const auto folder = store_->create_note_item({}, NoteItemKind::folder, "Research");
    const auto duplicate = store_->create_note_item({}, NoteItemKind::folder, "Alias");
    const auto mismatched = store_->create_note_item({}, NoteItemKind::folder, "Other key");
    const auto seed = [&](std::int64_t id, FolderStoreService service, const std::string& key, const std::string& remote) {
        const auto record = store_->register_folder_store(id, service, key, 1);
        store_->save_folder_store_remote_id(record, remote);
        store_->mark_folder_store_ready(record, "Ready");
        return record;
    };
    const auto original = seed(folder.id, FolderStoreService::openai, openai.id, "vs_shared");
    seed(duplicate.id, FolderStoreService::openai, openai.id, "vs_shared");
    seed(mismatched.id, FolderStoreService::openai, google.id, "vs_other");
    std::vector<GenerationRequest> requests;
    bool reject = true;
    Providers providers([&](auto) { return std::make_unique<FolderRequestBackend>(requests, reject); }, [](auto work) { work(); });
    const auto stored = sessions_->create("lobby", "Stores");
    auto opened = open_session(*sessions_, stored.identity, providers, std::make_shared<test::TestNotifier>(), *store_);
    auto& controller = *opened.controller;
    for (const auto id : {folder.id, duplicate.id, mismatched.id})
        ASSERT_TRUE(handle_text_input(controller, workspace_guest_id, note_item_ref_id(id)).clear_input);
    EXPECT_TRUE(requests.empty());
    const auto sent = controller.start_multicast(workspace_guest_id, "Question", {"Guide", "Writer"});
    ASSERT_TRUE(sent.notice);
    EXPECT_NE(sent.notice->find("different saved API key"), std::string::npos);
    EXPECT_NE(sent.notice->find("Writer"), std::string::npos);
    EXPECT_NE(sent.notice->find("/detach " + note_item_ref_id(folder.id)), std::string::npos);
    for (int i = 0; i < 10 && controller.is_generating(); ++i) (void)controller.receive_events(100);
    ASSERT_FALSE(controller.is_generating());
    ASSERT_EQ(requests.size(), 2u);
    ASSERT_TRUE(requests[0].folder_search);
    EXPECT_EQ(requests[0].folder_search->remote_store_ids, (std::vector<std::string>{"vs_shared"}));
    EXPECT_FALSE(requests[1].folder_search);
    bool saw_error = false, saw_writer = false;
    for (const auto& entry : controller.view().transcript.entries) {
        if (entry.text.find("Provider rejected store count") != std::string::npos) {
            saw_error = true;
            EXPECT_NE(entry.text.find("/detach " + note_item_ref_id(folder.id)), std::string::npos);
        }
        saw_writer |= entry.participant_id == "writer" && entry.text == "Answer";
    }
    EXPECT_TRUE(saw_error); EXPECT_TRUE(saw_writer);
    store_->delete_folder_store_record(original, "Removed");
    seed(folder.id, FolderStoreService::google, google.id, "fileSearchStores/new");
    (void)store_->rename_note_item(folder.id, "Renamed");
    (void)controller.submit_prompt(workspace_guest_id, "Next", "Writer");
    for (int i = 0; i < 10 && controller.is_generating(); ++i) (void)controller.receive_events(100);
    ASSERT_EQ(requests.size(), 3u);
    ASSERT_TRUE(requests.back().folder_search);
    EXPECT_EQ(requests.back().folder_search->service, FolderStoreService::google);
    EXPECT_EQ(requests.back().folder_search->remote_store_ids, (std::vector<std::string>{"fileSearchStores/new"}));
    // The earlier request keeps its captured IDs.
    EXPECT_EQ(requests[0].folder_search->remote_store_ids, (std::vector<std::string>{"vs_shared"}));
    for (const auto id : {folder.id, duplicate.id, mismatched.id})
        EXPECT_TRUE(handle_text_input(controller, workspace_guest_id, "/detach " + note_item_ref_id(id)).clear_input);
    (void)controller.submit_prompt(workspace_guest_id, "Recovered", "Guide");
    for (int i = 0; i < 10 && controller.is_generating(); ++i) (void)controller.receive_events(100);
    ASSERT_EQ(requests.size(), 4u);
    EXPECT_FALSE(requests.back().folder_search);
    EXPECT_EQ(controller.view().transcript.entries.back().text, "Answer");
}

TEST_F(SessionOpenTest, FolderAttachRechecksThePersistentJournalAfterCallbackAndRollsBackNotices) {
    const auto folder = store_->create_note_item({}, NoteItemKind::folder, "Research");
    const auto record = store_->register_folder_store(folder.id, FolderStoreService::openai, "saved", 1);
    store_->save_folder_store_remote_id(record, "vs_test");
    store_->mark_folder_store_ready(record, "Ready");
    const auto stored = sessions_->create("lobby", "Race");
    auto prepared = sessions_->prepare(stored.identity);
    bool invalidate = true;
    auto controller = SessionController::from_workspace([&] { return store_->snapshot(); },
        [&](auto id) { return store_->get_note_text(id, true); },
        [&](auto id) { return store_->get_note_item(id); }, [&](auto id) {
            auto current = store_->get_folder_store_by_folder(id);
            if (invalidate) store_->mark_folder_store_incomplete(record);
            return current;
        },
        "guide", std::string(workspace_guest_id),
        prepared.database_path, prepared.session_key, prepared.database_password, providers_,
        std::make_shared<test::TestNotifier>(), prepared.restore, prepared.identity, 0, {});
    EXPECT_FALSE(controller->add_item(folder.id).input_consumed);
    SessionJournal journal(prepared.database_path, prepared.session_key);
    EXPECT_TRUE(journal.folder_ids().empty());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    invalidate = false;
    store_->mark_folder_store_ready(record, "Ready");
    force_next_session_folder_notice_failure();
    EXPECT_FALSE(controller->add_item(folder.id).input_consumed);
    EXPECT_TRUE(journal.folder_ids().empty());
    EXPECT_TRUE(controller->add_item(folder.id).input_consumed);
    EXPECT_EQ(journal.folder_ids(), (std::vector<std::int64_t>{folder.id}));
}

TEST_F(SessionOpenTest, WelcomeMaintenanceSkipsFolderLookupAndReadyAttachUsesTheActiveVault) {
    const auto folder = store_->create_note_item({}, NoteItemKind::folder, "Research");
    const auto record = store_->register_folder_store(folder.id, FolderStoreService::openai, "saved", 1);
    store_->save_folder_store_remote_id(record, "vs_test"); store_->mark_folder_store_ready(record, "Ready");
    auto prepared = sessions_->prepare({std::string(entrance_id), std::string(welcome_id)});
    std::vector<GenerationRequest> requests;
    bool reject = false;
    Providers providers([&](auto) { return std::make_unique<FolderRequestBackend>(requests, reject); }, [](auto work) { work(); });
    int lookups = 0;
    auto controller = SessionController::from_workspace([&] { return store_->snapshot(); },
        [&](auto id) { return store_->get_note_text(id, true); },
        [&](auto id) { return store_->get_note_item(id); }, [&](auto id) {
            ++lookups; return store_->get_folder_store_by_folder(id);
        },
        std::string(workspace_assistant_id), std::string(workspace_guest_id),
        prepared.database_path, prepared.session_key, prepared.database_password, providers,
        std::make_shared<test::TestNotifier>(), prepared.restore, prepared.identity, 0, {});
    ASSERT_TRUE(controller->add_item(folder.id).input_consumed);
    EXPECT_TRUE(requests.empty());
    const auto before = lookups;
    (void)controller->submit_prompt(workspace_guest_id, "Maintenance");
    EXPECT_EQ(lookups, before);
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_TRUE(requests[0].maintenance_tool);
    EXPECT_FALSE(requests[0].folder_search);
}


TEST_F(SessionOpenTest, ProductionControllersRequireExplicitActiveVaultReaders) {
    const auto stored = sessions_->create("lobby", "Required readers");
    const auto prepared = sessions_->prepare(stored.identity);
    for (const bool missing_item : {false, true}) {
        SessionController::ItemReader read_item = [&](auto id) { return store_->get_note_item(id); };
        SessionController::StoreReader read_store = [&](auto id) { return store_->get_folder_store_by_folder(id); };
        if (missing_item) read_item = {};
        else read_store = {};
        EXPECT_THROW((void)SessionController::from_workspace([&] { return store_->snapshot(); },
            [&](auto id) { return store_->get_note_text(id, true); }, read_item, read_store,
            "guide", std::string(workspace_guest_id), prepared.database_path, prepared.session_key,
            prepared.database_password, providers_, std::make_shared<test::TestNotifier>(),
            prepared.restore, prepared.identity), std::invalid_argument);
    }
}

} // namespace
} // namespace cha
