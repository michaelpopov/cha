#include "session/session_open.h"

#include "providers/providers.h"
#include "providers/api_key_store.h"
#include "providers/google_api.h"
#include "runtime/text_input.h"
#include "session/session_controller.h"
#include "storage/folder_store_storage.h"
#include "storage/not_found_error.h"
#include "storage/sqlite_storage.h"
#include "storage/session_repository.h"
#include "storage/note_storage.h"
#include "storage/workspace_session_database.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"
#include "util/logging.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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

TEST_F(SessionOpenTest, WelcomeUsesDestinationVaultNotesAfterRetargetIncludingReusedAndMissingIds) {
    const auto document = [](std::string text) {
        return nlohmann::json{{"type", "doc"}, {"content", {{{"type", "paragraph"},
            {"content", {{{"type", "text"}, {"text", std::move(text)}}}}}}}};
    };
    const auto automatic = store_->create_note_item({}, NoteItemKind::note, "Automatic");
    const auto explicit_note = store_->create_note_item({}, NoteItemKind::note, "Explicit");
    const auto missing = store_->create_note_item({}, NoteItemKind::note, "Missing in B");
    for (const auto& item : {automatic, explicit_note, missing}) {
        store_->save_note(item.id, document("Vault A " + item.name));
        (void)store_->create_file_reference(FileReferenceOwner::character, workspace_assistant_id,
            FileReferenceKind::note, note_item_ref_id(item.id));
    }
    const FullSessionId welcome{std::string(entrance_id), std::string(welcome_id)};
    std::vector<SharedCharacterDefinition> definitions;
    std::vector<GenerationRequest> requests;
    bool reject = false;
    Providers providers([&](SharedCharacterDefinition definition) {
        definitions.push_back(std::move(definition));
        return std::make_unique<FolderRequestBackend>(requests, reject);
    }, [](auto work) { work(); });
    const auto send = [&] {
        auto opened = open_session(*sessions_, welcome, providers,
            std::make_shared<test::TestNotifier>(), *store_);
        const auto update = opened.controller->submit_prompt(workspace_guest_id, "Use the notes");
        EXPECT_TRUE(update.input_consumed);
        for (int i = 0; i < 10 && opened.controller->is_generating(); ++i)
            (void)opened.controller->receive_events(100);
        EXPECT_FALSE(opened.controller->is_generating());
        return update;
    };
    {
        auto opened = open_session(*sessions_, welcome, providers,
            std::make_shared<test::TestNotifier>(), *store_);
        ASSERT_TRUE(opened.controller->add_note(explicit_note.id).input_consumed);
        ASSERT_TRUE(opened.controller->add_note(missing.id).input_consumed);
        ASSERT_TRUE(opened.controller->add_text_file("keep.txt", "Retained file").input_consumed);
    }
    (void)send();
    ASSERT_EQ(definitions.size(), 1u);
    EXPECT_NE(definitions.back()->system_prompt.find("Vault A Automatic"), std::string::npos);
    const auto prepared = sessions_->prepare(welcome);
    const auto transcript_size = sessions_->history(welcome).size();
    {
        auto maintenance = sessions_->reserve_maintenance();
        maintenance.retarget(store_->database_path());
    }
    {
        SessionJournal journal(prepared.database_path, prepared.session_key);
        EXPECT_NE(journal.notes_prompt(workspace_assistant_id).find("Vault A Automatic"), std::string::npos);
        EXPECT_NE(journal.notes_prompt().find("Vault A Explicit"), std::string::npos);
    }
    const auto target = fixture_.root() / "destination.sqlite3";
    copy_workspace_session_database(database_, target);
    {
        storage::SqliteDatabase database(target, storage::SqliteDatabase::Mode::read_write);
        (void)note_save(database, automatic.id, document("Vault B Automatic"));
        (void)note_rename(database, automatic.id, "Destination name");
        note_save_compressed(database, explicit_note.id, "Vault B Explicit compressed");
        note_delete(database, missing.id);
    }
    {
        auto target_use = SessionLease::acquire(target, vault_in_use_message);
        auto database = store_->reserve_maintenance();
        auto repository = sessions_->reserve_maintenance();
        database.close();
        database.retarget(target, std::move(target_use));
        repository.retarget(target);
        database.reopen();
    }
    {
        SessionJournal journal(prepared.database_path, prepared.session_key);
        EXPECT_EQ(journal.notes_prompt(workspace_assistant_id), "# File: keep.txt\n\nRetained file");
        storage::SqliteDatabase database(prepared.database_path, storage::SqliteDatabase::Mode::read_only);
        EXPECT_FALSE(database.prepare("SELECT 1 FROM session_note_scopes").step());
    }
    EXPECT_EQ(sessions_->history(welcome).size(), transcript_size);
    const auto update = send();
    ASSERT_TRUE(update.notice);
    EXPECT_NE(update.notice->find("Missing in B (" + note_item_ref_id(missing.id) + "): note unavailable"), std::string::npos);
    ASSERT_EQ(definitions.size(), 2u);
    const auto& prompt = definitions.back()->system_prompt;
    EXPECT_NE(prompt.find("# Note: Destination name\n\nVault B Automatic"), std::string::npos);
    EXPECT_NE(prompt.find("# Note: Explicit\n\nVault B Explicit compressed"), std::string::npos);
    EXPECT_NE(prompt.find("# File: keep.txt\n\nRetained file"), std::string::npos);
    EXPECT_EQ(prompt.find("Vault A"), std::string::npos);
    // Prepared request definitions keep their own system prompt.
    EXPECT_NE(definitions.front()->system_prompt.find("Vault A Automatic"), std::string::npos);
}

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
    EXPECT_EQ(sent.notice->find("Writer"), std::string::npos);
    EXPECT_NE(sent.notice->find("/detach " + note_item_ref_id(mismatched.id)), std::string::npos);
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

class ReferencedFoldersTest : public SessionOpenTest {
protected:
    void SetUp() override {
        fixture_.add_character("writer", "Writer");
        std::filesystem::create_directories(fixture_.root() / "forums/lobby/members/writer");
        reimport_fixture();
        ApiKeyStore keys(*store_);
        openai_key = keys.create("OpenAI", "same-value").id;
        google_key = keys.create("Google", "same-value").id;
        config.host = "api.openai.com";
        config.port = 443;
        config.https = true;
        config.mode = Mode::net;
        config.api = ProviderApi::responses;
        config.model = "test";
        config.api_key_id = openai_key;
        update_provider();
        google_provider = store_->create_provider("Google", "test");
        auto google = config;
        google.host = "generativelanguage.googleapis.com";
        google.base_path = "/v1beta";
        google.api = ProviderApi::google;
        google.api_key_id = google_key;
        (void)store_->apply_provider_update(google_provider, "Google", google);
        (void)store_->apply_character_settings("writer", google_provider, {});
        providers = std::make_unique<Providers>([&](auto) {
            return std::make_unique<FolderRequestBackend>(requests, reject);
        }, [](auto work) { work(); });
        identity = sessions_->create("lobby", "Referenced folders").identity;
        initialize_diagnostic_logging(fixture_.root() / "folders.log", "info");
    }
    void TearDown() override {
        providers.reset();
        shutdown_diagnostic_logging();
    }
    void update_provider() {
        (void)store_->apply_provider_update("test", "Test", config);
    }
    NoteItem folder(std::string_view name, FolderStoreService service = FolderStoreService::openai,
                    std::string remote = {}) {
        const auto item = store_->create_note_item({}, NoteItemKind::folder, name);
        seed(item, service, remote.empty() ? "remote_" + std::to_string(item.id) : remote);
        return item;
    }
    std::int64_t seed(const NoteItem& item, FolderStoreService service, const std::string& remote) {
        const auto id = store_->register_folder_store(item.id, service,
            service == FolderStoreService::openai ? openai_key : google_key, 1);
        store_->save_folder_store_remote_id(id, remote);
        store_->mark_folder_store_ready(id, "Ready");
        return id;
    }
    void reference(const NoteItem& item, FileReferenceOwner owner = FileReferenceOwner::forum,
                   std::string_view id = "lobby") {
        (void)store_->create_file_reference(owner, id, FileReferenceKind::folder, note_item_ref_id(item.id));
    }
    void unreference(const NoteItem& item, FileReferenceOwner owner = FileReferenceOwner::forum,
                     std::string_view id = "lobby") {
        store_->delete_file_reference(owner, id, FileReferenceKind::folder, note_item_ref_id(item.id));
    }
    OpenedSession open_folders() {
        return open_session(*sessions_, identity, *providers, std::make_shared<test::TestNotifier>(), *store_);
    }
    std::unique_ptr<SessionController> open_with_readers(
        SessionController::ItemReader read_item, SessionController::StoreReader read_store) {
        const auto prepared = sessions_->prepare(identity);
        return SessionController::from_workspace([&] { return store_->snapshot(); },
            [&](auto id) { return store_->get_note_text(id, true); }, std::move(read_item), std::move(read_store),
            "guide", std::string(workspace_guest_id), prepared.database_path, prepared.session_key,
            prepared.database_password, *providers, std::make_shared<test::TestNotifier>(),
            prepared.restore, prepared.identity);
    }
    ControllerUpdate send(SessionController& controller, std::string target = "Guide") {
        auto update = controller.submit_prompt(workspace_guest_id, "Question", std::move(target));
        EXPECT_TRUE(update.input_consumed);
        drain(controller);
        return update;
    }
    void drain(SessionController& controller) {
        for (int i = 0; i < 10 && controller.is_generating(); ++i) (void)controller.receive_events(100);
        EXPECT_FALSE(controller.is_generating());
    }
    std::vector<std::string> ids() const {
        return requests.back().folder_search ? requests.back().folder_search->remote_store_ids
                                            : std::vector<std::string>{};
    }
    std::string log() const {
        std::ifstream input(fixture_.root() / "folders.log");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    std::string openai_key, google_key, google_provider;
    ModelBackendConfig config;
    std::unique_ptr<Providers> providers;
    std::vector<GenerationRequest> requests;
    bool reject{};
    FullSessionId identity;
};

TEST_F(ReferencedFoldersTest, MulticastUnionsApplicableSourcesAndSilentlyFiltersOtherServices) {
    const auto a = folder("A", FolderStoreService::openai, "vs_a");
    const auto b = folder("B", FolderStoreService::google, "fileSearchStores/b");
    const auto c = folder("C", FolderStoreService::openai, "vs_c");
    const auto d = folder("D", FolderStoreService::google, "fileSearchStores/d");
    const auto e = folder("E", FolderStoreService::openai, "vs_e");
    reference(a); reference(b);
    reference(a, FileReferenceOwner::character, "guide");
    reference(c, FileReferenceOwner::character, "guide");
    reference(d, FileReferenceOwner::character, "writer");
    auto opened = open_folders();
    ASSERT_TRUE(opened.controller->add_item(e.id).input_consumed);
    ASSERT_TRUE(opened.controller->add_item(b.id).input_consumed);
    const auto update = opened.controller->start_multicast(workspace_guest_id, "Question", {"Guide", "Writer"});
    drain(*opened.controller);
    ASSERT_EQ(requests.size(), 2u);
    ASSERT_TRUE(requests[0].folder_search);
    ASSERT_TRUE(requests[1].folder_search);
    EXPECT_TRUE(requests[0].summary_tool);
    EXPECT_TRUE(requests[1].summary_tool);
    EXPECT_EQ(requests[0].folder_search->remote_store_ids, (std::vector<std::string>{"vs_a", "vs_e"}));
    EXPECT_EQ(requests[1].folder_search->remote_store_ids,
        (std::vector<std::string>{"fileSearchStores/b", "fileSearchStores/d"}));
    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Folder access omitted: Guide — C (" + note_item_ref_id(c.id)
        + "): two-store OpenAI limit; sources: Guide Files.");
    EXPECT_EQ(requests[0].folder_search_context, "A (" + note_item_ref_id(a.id)
        + "); sources: Forum The Lobby Files, Guide Files; E (" + note_item_ref_id(e.id)
        + "); sources: chat (/detach " + note_item_ref_id(e.id) + ")");
    EXPECT_EQ(log().find("different service"), std::string::npos);
}

TEST_F(ReferencedFoldersTest, CurrentSettingsDetachClearAndReopenKeepEachSourceIndependent) {
    const auto a = folder("A", FolderStoreService::openai, "vs_a");
    const auto b = folder("B", FolderStoreService::openai, "vs_b");
    reference(a);
    auto opened = open_folders();
    auto* controller = opened.controller.get();
    (void)send(*controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_a"}));
    reference(b, FileReferenceOwner::character, "guide");
    (void)send(*controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_a", "vs_b"}));
    ASSERT_TRUE(controller->add_item(a.id).input_consumed);
    unreference(a);
    (void)send(*controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_a", "vs_b"}));
    unreference(b, FileReferenceOwner::character, "guide");
    opened = {};
    opened = open_folders();
    controller = opened.controller.get();
    (void)send(*controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_a"}));
    reference(a);
    ASSERT_TRUE(controller->detach_folder(a.id).input_consumed);
    (void)send(*controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_a"}));
    EXPECT_EQ(requests.back().folder_search_context.find("/detach"), std::string::npos);
    ASSERT_TRUE(controller->detach_folder(a.id).input_consumed);
    EXPECT_NE(controller->view().transcript.entries.back().text.find("is not attached"), std::string::npos);
    ASSERT_TRUE(controller->add_item(b.id).input_consumed);
    ASSERT_TRUE(controller->clear_conversation().input_consumed);
    (void)send(*controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_a"}));
    const auto prepared = sessions_->prepare(identity);
    SessionJournal journal(prepared.database_path, prepared.session_key);
    EXPECT_TRUE(journal.folder_ids().empty());
}

TEST_F(ReferencedFoldersTest, ResolvesCurrentStoresEffectiveProviderAndSavedKeyOnEveryAnswer) {
    const auto a = folder("A", FolderStoreService::openai, "vs_old");
    reference(a);
    auto opened = open_folders();
    (void)send(*opened.controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_old"}));
    store_->delete_folder_store_record(store_->get_folder_store_by_folder(a.id)->id, "Removed");
    auto update = send(*opened.controller);
    EXPECT_TRUE(ids().empty());
    ASSERT_TRUE(update.notice);
    EXPECT_NE(update.notice->find("no ready store"), std::string::npos);
    seed(a, FolderStoreService::openai, "vs_new");
    (void)send(*opened.controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_new"}));
    EXPECT_EQ(requests.front().folder_search->remote_store_ids, (std::vector<std::string>{"vs_old"}));
    config.api_key_id = google_key; // Equal key values under different saved IDs are ineligible.
    update_provider();
    update = send(*opened.controller);
    EXPECT_TRUE(ids().empty());
    ASSERT_TRUE(update.notice);
    EXPECT_NE(update.notice->find("different saved API key"), std::string::npos);
    store_->delete_folder_store_record(store_->get_folder_store_by_folder(a.id)->id, "Removed");
    seed(a, FolderStoreService::google, "fileSearchStores/new");
    update = send(*opened.controller);
    EXPECT_TRUE(ids().empty());
    EXPECT_TRUE(!update.notice || update.notice->empty());
    EXPECT_FALSE(opened.controller->set_character_settings("guide", google_provider, {}).notice);
    (void)send(*opened.controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"fileSearchStores/new"}));
}

TEST_F(ReferencedFoldersTest, OpenAiCapacityMergesRemoteSourcesAndPreservesNumericOrder) {
    const auto a = folder("A", FolderStoreService::openai, "vs_shared");
    const auto b = folder("B", FolderStoreService::openai, "vs_b");
    const auto c = folder("C", FolderStoreService::openai, "vs_c");
    const auto alias = folder("Alias", FolderStoreService::openai, "vs_shared");
    const auto alias_c = folder("Alias C", FolderStoreService::openai, "vs_c");
    // Reverse settings order does not affect tie-breaking or payload order.
    reference(c); reference(b); reference(a); reference(alias_c);
    reference(c, FileReferenceOwner::character, "guide");
    reference(b, FileReferenceOwner::character, "guide");
    auto opened = open_folders();
    ASSERT_TRUE(opened.controller->add_item(alias.id).input_consumed);
    auto update = send(*opened.controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_shared", "vs_b"}));
    ASSERT_TRUE(update.notice);
    EXPECT_NE(update.notice->find("C (" + note_item_ref_id(c.id)
        + "): two-store OpenAI limit; sources: Forum The Lobby Files, Guide Files"), std::string::npos);
    EXPECT_NE(update.notice->find("Alias C (" + note_item_ref_id(alias_c.id)
        + "): two-store OpenAI limit; sources: Forum The Lobby Files"), std::string::npos);
    unreference(b);
    reference(a, FileReferenceOwner::character, "guide");
    (void)send(*opened.controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_shared", "vs_b"}));
    ASSERT_TRUE(opened.controller->detach_folder(alias.id).input_consumed);
    unreference(a, FileReferenceOwner::character, "guide");
    (void)send(*opened.controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_b", "vs_c"}));
    ASSERT_TRUE(opened.controller->add_item(c.id).input_consumed);
    (void)send(*opened.controller);
    // Higher priority C stays after B in the serialized payload.
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_b", "vs_c"}));
}

TEST_F(ReferencedFoldersTest, GoogleKeepsAllThreeStoresAndFiltersForeignFailuresBeforeReadinessOrKeys) {
    const auto a = folder("A", FolderStoreService::google, "fileSearchStores/a");
    const auto b = folder("B", FolderStoreService::google, "fileSearchStores/b");
    const auto c = folder("C", FolderStoreService::google, "fileSearchStores/c");
    const auto other = folder("Other");
    reference(c); reference(a);
    reference(b, FileReferenceOwner::character, "writer");
    reference(other);
    reference(other, FileReferenceOwner::character, "writer");
    auto opened = open_folders();
    ASSERT_TRUE(opened.controller->add_item(other.id).input_consumed);
    store_->mark_folder_store_incomplete(store_->get_folder_store_by_folder(other.id)->id);
    const auto update = send(*opened.controller, "Writer");
    EXPECT_EQ(ids(), (std::vector<std::string>{"fileSearchStores/a", "fileSearchStores/b", "fileSearchStores/c"}));
    EXPECT_TRUE(!update.notice || update.notice->empty());
    EXPECT_EQ(log().find("Other ("), std::string::npos);
}

TEST_F(ReferencedFoldersTest, ConnectionFailureKeepsForumOnlyLogOnlyAndCombinesDeliberateSources) {
    const auto inherited = folder("Inherited");
    const auto shared = folder("Shared");
    const auto own = folder("Own");
    const auto foreign = folder("Foreign", FolderStoreService::google);
    const auto chat_only = folder("Chat only");
    reference(inherited);
    config.api = ProviderApi::chat_completions;
    update_provider();
    auto opened = open_folders();
    auto update = send(*opened.controller);
    EXPECT_TRUE(ids().empty());
    EXPECT_TRUE(!update.notice || update.notice->empty());
    EXPECT_NE(log().find("this connection does not support folder search; Inherited"), std::string::npos);
    reference(shared);
    reference(shared, FileReferenceOwner::character, "guide");
    reference(own, FileReferenceOwner::character, "guide");
    reference(foreign, FileReferenceOwner::character, "guide");
    ASSERT_TRUE(opened.controller->add_item(shared.id).input_consumed);
    ASSERT_TRUE(opened.controller->add_item(chat_only.id).input_consumed);
    update = send(*opened.controller);
    ASSERT_TRUE(update.notice);
    EXPECT_TRUE(update.notice->starts_with("Folder access omitted: Guide — this connection does not support folder search;"));
    EXPECT_EQ(update.notice->find("Inherited"), std::string::npos);
    EXPECT_NE(update.notice->find("Foreign"), std::string::npos); // Unknown endpoint cannot infer service.
    EXPECT_NE(update.notice->find("Forum The Lobby Files, Guide Files, chat (/detach"), std::string::npos);
    EXPECT_NE(update.notice->find("Chat only (" + note_item_ref_id(chat_only.id) + "): chat (/detach "),
        std::string::npos);
    config.api = ProviderApi::responses;
    config.api_key_id = "missing";
    update_provider();
    update = send(*opened.controller);
    ASSERT_TRUE(update.notice);
    EXPECT_NE(update.notice->find("The saved API key was not found"), std::string::npos);
    EXPECT_EQ(update.notice->find("Foreign"), std::string::npos);
    EXPECT_NE(update.notice->find("Own"), std::string::npos);
    config.auth = ProviderAuth::openai_subscription;
    config.host = "chatgpt.com";
    config.base_path = "/backend-api/codex";
    config.api_key_id.clear();
    config.cache_retention = CacheRetention::off;
    update_provider();
    update = send(*opened.controller);
    ASSERT_TRUE(update.notice);
    EXPECT_NE(update.notice->find("this connection does not support folder search"), std::string::npos);
    EXPECT_NE(update.notice->find("Foreign"), std::string::npos);
}

TEST_F(ReferencedFoldersTest, RenamesWarnForEveryOwnerAndProviderErrorsIdentifyAllSources) {
    const auto a = folder("Captured", FolderStoreService::openai, "vs_a");
    reference(a);
    reference(a, FileReferenceOwner::character, "guide");
    auto opened = open_folders();
    ASSERT_TRUE(opened.controller->add_item(a.id).input_consumed);
    (void)store_->rename_note_item(a.id, "Current");
    reject = true;
    const auto update = send(*opened.controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"vs_a"}));
    EXPECT_TRUE(!update.notice || update.notice->empty());
    const auto output = log();
    EXPECT_NE(output.find("Folder reference name changed in lobby/" + identity.session_id), std::string::npos);
    EXPECT_NE(output.find("Forum The Lobby Files (" + note_item_ref_id(a.id) + "): captured “Captured”, current “Current”"), std::string::npos);
    EXPECT_NE(output.find("Guide Files (" + note_item_ref_id(a.id) + "): captured “Captured”, current “Current”"), std::string::npos);
    const auto error = opened.controller->view().transcript.entries.back().text;
    EXPECT_NE(error.find("Folder access: Current (" + note_item_ref_id(a.id)
        + "); sources: Forum The Lobby Files, Guide Files, chat (/detach " + note_item_ref_id(a.id) + ")"), std::string::npos);
    EXPECT_EQ(requests.back().run.prompt_text.find("Files"), std::string::npos);
}

TEST_F(ReferencedFoldersTest, BadSourcesAreOmittedOnceAndValidPeersStillUseCapacity) {
    std::vector<NoteItem> folders;
    for (const auto name : {"Missing", "Wrong kind", "Item failure", "Store failure", "Absent",
            "Incomplete", "Missing remote", "Empty remote", "Wrong key", "Valid A", "Valid B"}) {
        folders.push_back(folder(name));
        reference(folders.back());
        reference(folders.back(), FileReferenceOwner::character, "guide");
    }
    std::map<std::int64_t, int> item_reads, store_reads;
    auto controller = open_with_readers([&](auto id) -> std::optional<NoteItem> {
        ++item_reads[id];
        if (id == folders[0].id) return {};
        if (id == folders[2].id) throw std::runtime_error("injected item failure");
        auto item = store_->get_note_item(id);
        if (id == folders[1].id) item->kind = NoteItemKind::note;
        return item;
    }, [&](auto id) -> std::optional<FolderStoreRecord> {
        ++store_reads[id];
        if (id == folders[3].id) throw std::runtime_error("injected store failure");
        if (id == folders[4].id) return {};
        auto record = store_->get_folder_store_by_folder(id);
        if (id == folders[5].id) record->state = FolderStoreState::incomplete;
        if (id == folders[6].id) record->remote_store_id.reset();
        if (id == folders[7].id) record->remote_store_id = "";
        if (id == folders[8].id) record->api_key_id = google_key;
        return record;
    });
    const auto update = send(*controller);
    EXPECT_EQ(ids(), (std::vector<std::string>{"remote_" + std::to_string(folders[9].id),
        "remote_" + std::to_string(folders[10].id)}));
    ASSERT_TRUE(update.notice);
    const std::vector<std::string> reasons{"folder unavailable", "folder unavailable", "could not read folder store",
        "could not read folder store", "no ready store", "no ready store", "no ready store", "no ready store",
        "different saved API key"};
    for (std::size_t i = 0; i < folders.size(); ++i) {
        EXPECT_EQ(item_reads[folders[i].id], 1);
        EXPECT_EQ(store_reads[folders[i].id], i < 3 ? 0 : 1);
        if (i >= reasons.size()) continue;
        const auto entry = folders[i].name + " (" + note_item_ref_id(folders[i].id) + "): " + reasons[i]
            + "; sources: Forum The Lobby Files, Guide Files";
        const auto pos = update.notice->find(entry);
        EXPECT_NE(pos, std::string::npos) << entry;
        if (pos != std::string::npos) EXPECT_EQ(update.notice->find(entry, pos + 1), std::string::npos);
    }
    EXPECT_EQ(update.notice->find("two-store OpenAI limit"), std::string::npos);
}

TEST_F(ReferencedFoldersTest, FailedNativeConnectionReadsOnlyDeliberateStoresAndKeepsUnknownServices) {
    const auto inherited = folder("Inherited");
    const auto foreign = folder("Foreign", FolderStoreService::google);
    const auto absent = folder("Absent");
    const auto broken = folder("Broken");
    reference(inherited);
    for (const auto& item : {foreign, absent, broken}) reference(item, FileReferenceOwner::character, "guide");
    store_->delete_folder_store_record(store_->get_folder_store_by_folder(absent.id)->id, "Removed");
    config.api_key_id = "missing";
    update_provider();
    std::map<std::int64_t, int> reads, item_reads;
    auto controller = open_with_readers([&](auto id) {
        ++item_reads[id];
        return store_->get_note_item(id);
    }, [&](auto id) -> std::optional<FolderStoreRecord> {
        ++reads[id];
        if (id == broken.id) throw std::runtime_error("injected lookup failure");
        return store_->get_folder_store_by_folder(id);
    });
    const auto update = send(*controller);
    EXPECT_TRUE(ids().empty());
    EXPECT_EQ(reads[inherited.id], 0);
    EXPECT_EQ(item_reads[inherited.id], 0);
    EXPECT_EQ(reads[foreign.id], 1);
    EXPECT_EQ(reads[absent.id], 1);
    EXPECT_EQ(reads[broken.id], 1);
    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Folder access omitted: Guide — The saved API key was not found; Absent ("
        + note_item_ref_id(absent.id) + "): Guide Files; Broken (" + note_item_ref_id(broken.id) + "): Guide Files.");
    EXPECT_NE(log().find("could not read folder store"), std::string::npos);
    unreference(absent, FileReferenceOwner::character, "guide");
    unreference(broken, FileReferenceOwner::character, "guide");
    const auto filtered = send(*controller);
    EXPECT_TRUE(!filtered.notice || filtered.notice->empty());
}

TEST_F(ReferencedFoldersTest, SessionNamingDoesNotResolveOrInheritOrdinaryFolderAccess) {
    const auto a = folder("A", FolderStoreService::openai, "vs_a");
    reference(a);
    reference(a, FileReferenceOwner::character, "guide");
    store_->apply_session_naming_update({"test", "low"});
    int item_reads = 0, store_reads = 0;
    auto controller = open_with_readers([&](auto id) {
        ++item_reads;
        return store_->get_note_item(id);
    }, [&](auto id) {
        ++store_reads;
        return store_->get_folder_store_by_folder(id);
    });
    controller->enable_auto_naming("New session");
    (void)send(*controller);
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(item_reads, 1);
    EXPECT_EQ(store_reads, 1);
    EXPECT_EQ(requests[0].run.target.id, "guide");
    EXPECT_TRUE(requests[0].summary_tool);
    ASSERT_TRUE(requests[0].folder_search);
    EXPECT_EQ(requests[0].folder_search->remote_store_ids, (std::vector<std::string>{"vs_a"}));
    EXPECT_EQ(requests[1].run.target.id, "session-name");
    EXPECT_FALSE(requests[1].folder_search);
    EXPECT_TRUE(requests[1].folder_search_context.empty());
}

TEST_F(ReferencedFoldersTest, InheritedGoogleFoldersKeepGemini25FileSearchAndWarnWhenFunctionsAreRemoved) {
    const auto a = folder("A", FolderStoreService::google, "fileSearchStores/a");
    reference(a);
    auto google = store_->snapshot()->find_provider(google_provider)->config;
    google.model = "gemini-2.5-flash";
    (void)store_->apply_provider_update(google_provider, "Google", google);
    auto opened = open_folders();
    const auto update = send(*opened.controller, "Writer");
    EXPECT_TRUE(!update.notice || update.notice->empty());
    ASSERT_TRUE(requests.back().folder_search);
    ASSERT_TRUE(requests.back().summary_tool);
    const auto body = nlohmann::json::parse(build_google_request_body(requests.back(), google, "System"));
    EXPECT_EQ(body["tools"], (nlohmann::json::array({{{"fileSearch",
        {{"fileSearchStoreNames", {"fileSearchStores/a"}}}}}})));
    EXPECT_NE(log().find("Omitting optional CHA custom functions: gemini-2.5-flash"), std::string::npos);
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
    storage::SqliteDatabase failure_db(prepared.database_path, storage::SqliteDatabase::Mode::read_write);
    failure_db.execute("CREATE TRIGGER reject_folder_notice BEFORE INSERT ON entries BEGIN SELECT RAISE(ABORT, 'notice failure'); END");
    EXPECT_FALSE(controller->add_item(folder.id).input_consumed);
    EXPECT_TRUE(journal.folder_ids().empty());
    failure_db.execute("DROP TRIGGER reject_folder_notice");
    EXPECT_TRUE(controller->add_item(folder.id).input_consumed);
    EXPECT_EQ(journal.folder_ids(), (std::vector<std::int64_t>{folder.id}));
}

TEST_F(SessionOpenTest, WelcomeMaintenanceSkipsFolderLookupAndReadyAttachUsesTheActiveVault) {
    const auto folder = store_->create_note_item({}, NoteItemKind::folder, "Research");
    const auto record = store_->register_folder_store(folder.id, FolderStoreService::openai, "saved", 1);
    store_->save_folder_store_remote_id(record, "vs_test"); store_->mark_folder_store_ready(record, "Ready");
    (void)store_->create_file_reference(FileReferenceOwner::character, workspace_assistant_id,
        FileReferenceKind::folder, note_item_ref_id(folder.id));
    auto prepared = sessions_->prepare({std::string(entrance_id), std::string(welcome_id)});
    std::vector<GenerationRequest> requests;
    bool reject = false;
    Providers providers([&](auto) { return std::make_unique<FolderRequestBackend>(requests, reject); }, [](auto work) { work(); });
    int lookups = 0;
    auto controller = SessionController::from_workspace([&] { return store_->snapshot(); },
        [&](auto id) { return store_->get_note_text(id, true); },
        [&](auto id) { ++lookups; return store_->get_note_item(id); }, [&](auto id) {
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
