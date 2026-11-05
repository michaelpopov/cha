#include "session/session_open.h"

#include "providers/providers.h"
#include "runtime/text_input.h"
#include "runtime/session_projection.h"
#include "session/session_controller.h"
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

#include <algorithm>
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


class LibraryRequestBackend final : public ModelBackend {
public:
    LibraryRequestBackend(std::vector<GenerationRequest>& requests, bool& reject)
        : requests_(requests), reject_(reject) {}
    RequestPayload prepare(const GenerationRequest& input) override {
        requests_.push_back(input);
        return {};
    }
    GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink, const std::atomic_bool&) override {
        if (reject_ && !requests_.back().library_store_ids.empty() && requests_.back().run.target.id == "guide")
            return {GenerationOutcome::protocol_error, "Provider rejected store count"};
        sink({GenerationDeltaKind::answer, "Answer"});
        return {};
    }
private:
    std::vector<GenerationRequest>& requests_;
    bool& reject_;
};


class ForumLibraryTest : public SessionOpenTest {
protected:
    void SetUp() override {
        fixture_.add_character("writer", "Writer");
        std::filesystem::create_directories(fixture_.root() / "forums/lobby/members/writer");
        std::filesystem::create_directories(fixture_.root() / "forums/lobby/members/builtin-assistant");
        fixture_.add_forum("other", "Other", "guide");
        fixture_.write_character_config("display_name = 'Guide'\nprovider = 'test'\nreasoning_effort = 'high'\nweb_search = 'off'\n");
        reimport_fixture();
        store_->apply_api_key_create("api_key_1", "OpenAI", "same-secret", "openai");
        store_->apply_api_key_create("api_key_2", "Google", "same-secret", "google");
        store_->apply_api_key_create("api_key_3", "Other OpenAI", "same-secret", "openai");
        config.host = "api.openai.com";
        config.port = 443;
        config.https = true;
        config.mode = Mode::net;
        config.api = ProviderApi::responses;
        config.model = "openai-model";
        config.api_key_id = "api_key_1";
        config.reasoning_effort = "low";
        config.web_search = WebSearchMode::automatic;
        (void)store_->apply_provider_update("test", "OpenAI", config);
        google_provider = store_->create_provider("Google", "test");
        auto google = config;
        google.host = "generativelanguage.googleapis.com";
        google.base_path = "/v1beta";
        google.api = ProviderApi::google;
        google.api_key_id = "api_key_2";
        google.model = "gemini-3-flash";
        google.web_search = WebSearchMode::off;
        (void)store_->apply_provider_update(google_provider, "Google", google);
        (void)store_->apply_character_settings("writer", google_provider, {});
        providers = std::make_unique<Providers>([&](auto definition) {
            definitions.push_back(definition);
            if (on_prepare) on_prepare();
            return std::make_unique<LibraryRequestBackend>(requests, reject);
        }, [&](auto work) {
            if (defer) pending.push_back(std::move(work));
            else work();
        });
        identity = sessions_->create("lobby", "Library").identity;
        initialize_diagnostic_logging(fixture_.root() / "library.log", "info");
    }
    void TearDown() override {
        for (auto& work : pending) work();
        pending.clear();
        providers.reset();
        shutdown_diagnostic_logging();
    }
    std::unique_ptr<SessionController> controller(const FullSessionId& id) {
        const auto prepared = sessions_->prepare(id);
        return SessionController::from_workspace([&] { return store_->snapshot(); },
            [&](auto item) { return store_->get_note_text(item, true); },
            [&](auto item) { return store_->get_note_item(item); },
            "guide", std::string(workspace_guest_id), prepared.database_path, prepared.session_key,
            prepared.database_password, *providers, std::make_shared<test::TestNotifier>(),
            prepared.restore, prepared.identity);
    }
    void library(std::vector<std::string> ids = {"vs_a", "vs_b"},
                 std::optional<std::string> provider = {}, std::string key = "api_key_1") {
        (void)store_->apply_forum_library("lobby", ForumLibrary{std::move(key), std::move(ids), std::move(provider)});
    }
    void drain(SessionController& c) {
        for (int i = 0; i < 10 && c.is_generating(); ++i) (void)c.receive_events(100);
        EXPECT_FALSE(c.is_generating());
    }
    void send(SessionController& c, std::string target = "Guide") {
        if (target == "*") {
            (void)c.set_default_character_by_id("*");
            target.clear();
        }
        EXPECT_TRUE(c.submit_prompt(workspace_guest_id, "Question", std::move(target)).input_consumed);
        drain(c);
    }
    SessionSnapshot snapshot(const SessionController& c) {
        return to_snapshot(*store_->snapshot(), identity, "Library", c.view(), {});
    }
    std::string log() {
        std::ifstream input(fixture_.root() / "library.log");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    ModelBackendConfig config;
    std::string google_provider;
    FullSessionId identity;
    std::unique_ptr<Providers> providers;
    std::vector<GenerationRequest> requests;
    std::vector<SharedCharacterDefinition> definitions;
    std::function<void()> on_prepare;
    std::vector<std::function<void()>> pending;
    bool reject{}, defer{};
};

TEST_F(ForumLibraryTest, MixedMulticastAndAssistantUseNativeLibraryWithoutAttachments) {
    library();
    auto c = controller(identity);
    send(*c, "*");
    ASSERT_EQ(requests.size(), 3u);
    for (const auto& request : requests) {
        EXPECT_EQ(request.library_store_ids, request.run.target.id == "writer"
            ? std::vector<std::string>{} : (std::vector<std::string>{"vs_a", "vs_b"}));
    }
    EXPECT_NE(log().find("Effective provider cannot use the Library key"), std::string::npos);
    EXPECT_EQ(log().find("same-secret"), std::string::npos);
    const auto other = sessions_->create("other", "Isolated").identity;
    auto second = controller(other);
    send(*second);
    EXPECT_TRUE(requests.back().library_store_ids.empty());
    // Attachment and detachment do not select stores.
    const auto folder = store_->create_note_item({}, NoteItemKind::folder, "Unrelated");
    ASSERT_TRUE(c->add_item(folder.id).input_consumed);
    send(*c);
    EXPECT_EQ(requests.back().library_store_ids, (std::vector<std::string>{"vs_a", "vs_b"}));
    ASSERT_TRUE(c->detach_folder(folder.id).input_consumed);
    send(*c);
    EXPECT_EQ(requests.back().library_store_ids, (std::vector<std::string>{"vs_a", "vs_b"}));
}

TEST_F(ForumLibraryTest, DefaultMasksSessionChoicesAndPreservesEffortAndCharacterSettings) {
    library({"vs_a"}, "test");
    auto c = controller(identity);
    (void)c->set_character_settings("guide", std::optional<std::string>(google_provider), std::optional<std::string>("xhigh"));
    (void)c->set_character_settings("writer", std::optional<std::string>(google_provider), std::nullopt);
    auto projected = snapshot(*c);
    EXPECT_TRUE(projected.chat_settings.model_locked);
    for (const auto& selected : projected.chat_settings.characters) EXPECT_EQ(selected.provider_id, "test");
    send(*c, "*");
    ASSERT_EQ(definitions.size(), 3u);
    for (const auto& definition : definitions) {
        EXPECT_EQ(definition->provider.id, "test");
        EXPECT_EQ(definition->provider.config.model, "openai-model");
        EXPECT_EQ(definition->provider.config.api_key_id, "api_key_1");
        if (definition->character.id == "guide") {
            EXPECT_EQ(definition->provider.config.reasoning_effort, "xhigh");
            EXPECT_EQ(definition->provider.config.web_search, WebSearchMode::off);
        }
    }
    EXPECT_EQ(store_->snapshot()->find_character("writer")->provider_id, google_provider);
    EXPECT_EQ(c->view().character_settings->at("guide").provider_id, google_provider);
    library({"vs_a"});
    EXPECT_FALSE(snapshot(*c).chat_settings.model_locked);
    send(*c);
    EXPECT_EQ(definitions.back()->provider.id, google_provider);
    EXPECT_TRUE(requests.back().library_store_ids.empty());
    library({"fileSearchStores/one", "fileSearchStores/two"}, google_provider, "api_key_2");
    send(*c, "*");
    for (std::size_t i = requests.size() - 3; i < requests.size(); ++i) {
        EXPECT_EQ(definitions[i]->provider.id, google_provider);
        EXPECT_EQ(requests[i].library_store_ids, (std::vector<std::string>{"fileSearchStores/one", "fileSearchStores/two"}));
    }
}

TEST_F(ForumLibraryTest, InvalidStoresKeepValidDefaultAndUseSharedValidationMessage) {
    auto c = controller(identity);
    for (const auto& ids : std::vector<std::vector<std::string>>{
            {}, {"vs_a", "vs_b", "vs_c"}, {"vs_a", "vs_a"}, {"../bad"}}) {
        library(ids, "test");
        const auto current = store_->snapshot();
        ASSERT_NE(current->forum_default_provider("lobby"), nullptr);
        ASSERT_TRUE(current->find_forum("lobby")->library);
        const auto problem = forum_library_problem(*current, *current->find_forum("lobby")->library);
        ASSERT_TRUE(problem);
        EXPECT_TRUE(snapshot(*c).chat_settings.model_locked);
        send(*c, "Writer");
        EXPECT_EQ(definitions.back()->provider.id, "test");
        EXPECT_TRUE(requests.back().library_store_ids.empty());
        EXPECT_NE(log().find(*problem), std::string::npos);
    }
}

TEST_F(ForumLibraryTest, StaleDefaultIsSilentInSnapshotsAndWarnsOncePerGenerationTarget) {
    library({"vs_a"}, "missing");
    auto c = controller(identity);
    auto before = log();
    for (int i = 0; i < 3; ++i) EXPECT_FALSE(snapshot(*c).chat_settings.model_locked);
    EXPECT_EQ(log(), before);
    send(*c, "*");
    const auto warnings = log();
    std::size_t count = 0, pos = 0;
    while ((pos = warnings.find("Ignoring stale Forum default", pos)) != std::string::npos) { ++count; ++pos; }
    EXPECT_EQ(count, 3u);
    EXPECT_NE(warnings.find("session=" + identity.session_id), std::string::npos);
    EXPECT_EQ(definitions.back()->provider.id, google_provider);
    // A stale default does not suppress access by compatible normal providers.
    EXPECT_EQ(requests.front().library_store_ids, (std::vector<std::string>{"vs_a"}));
    library({"vs_a"}, "test", "missing-key");
    send(*c);
    EXPECT_FALSE(snapshot(*c).chat_settings.model_locked);
    EXPECT_TRUE(requests.back().library_store_ids.empty());
    library({"vs_a"}, "test");
    store_->apply_api_key_delete("api_key_1");
    send(*c);
    EXPECT_FALSE(snapshot(*c).chat_settings.model_locked);
    EXPECT_TRUE(requests.back().library_store_ids.empty());
}

TEST_F(ForumLibraryTest, AllChildrenAndPreparedRepliesRetainTheirStartingRevision) {
    library({"vs_old"}, "test");
    auto c = controller(identity);
    defer = true;
    on_prepare = [&] {
        if (definitions.size() == 1) library({"fileSearchStores/new"}, google_provider, "api_key_2");
    };
    (void)c->set_default_character_by_id("*");
    ASSERT_TRUE(c->submit_prompt(workspace_guest_id, "Question").input_consumed);
    ASSERT_TRUE(c->is_generating());
    ASSERT_EQ(pending.size(), 3u);
    pending.front()();
    (void)store_->apply_forum_library("lobby", std::nullopt);
    for (std::size_t i = 1; i < pending.size(); ++i) pending[i]();
    pending.clear();
    ASSERT_EQ(requests.size(), 3u);
    for (std::size_t i = 0; i < requests.size(); ++i) {
        EXPECT_EQ(definitions[i]->provider.id, "test");
        EXPECT_EQ(requests[i].library_store_ids, (std::vector<std::string>{"vs_old"}));
    }
    drain(*c);
    defer = false;
    on_prepare = {};
    send(*c);
    EXPECT_TRUE(requests.back().library_store_ids.empty());
    EXPECT_EQ(requests.front().library_store_ids, (std::vector<std::string>{"vs_old"}));
    library({"fileSearchStores/new"}, google_provider, "api_key_2");
    c.reset();
    c = controller(identity);
    send(*c);
    EXPECT_EQ(definitions.back()->provider.id, google_provider);
    EXPECT_EQ(requests.back().library_store_ids, (std::vector<std::string>{"fileSearchStores/new"}));
}

TEST_F(ForumLibraryTest, NamingAndProviderFailureKeepExistingBoundaries) {
    library({"fileSearchStores/one"}, google_provider, "api_key_2");
    auto c = controller(identity);
    c->enable_auto_naming("New session");
    reject = true;
    send(*c);
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].library_store_ids, (std::vector<std::string>{"fileSearchStores/one"}));
    EXPECT_EQ(definitions[0]->provider.id, google_provider);
    EXPECT_TRUE(requests[1].library_store_ids.empty());
    EXPECT_EQ(definitions[1]->provider.config.model, store_->snapshot()->find_provider("test")->config.model);
    const auto& entries = c->view().transcript.entries;
    EXPECT_TRUE(std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.kind == EntryKind::error && entry.text.find("Provider rejected store count") != std::string::npos;
    }));
    EXPECT_EQ(log().find("same-secret"), std::string::npos);
}

TEST_F(SessionOpenTest, FolderAttachNeedsNoStoreAndRollsBackWhenItsNoticeFails) {
    const auto folder = store_->create_note_item({}, NoteItemKind::folder, "Research");
    const auto stored = sessions_->create("lobby", "Race");
    const auto prepared = sessions_->prepare(stored.identity);
    auto opened = open(stored.identity);
    const auto& controller = opened.controller;
    SessionJournal journal(prepared.database_path, prepared.session_key);
    storage::SqliteDatabase failure_db(prepared.database_path, storage::SqliteDatabase::Mode::read_write);
    failure_db.execute("CREATE TRIGGER reject_folder_notice BEFORE INSERT ON entries BEGIN SELECT RAISE(ABORT, 'notice failure'); END");
    EXPECT_FALSE(controller->add_item(folder.id).input_consumed);
    EXPECT_TRUE(journal.folder_ids().empty());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    failure_db.execute("DROP TRIGGER reject_folder_notice");
    EXPECT_TRUE(controller->add_item(folder.id).input_consumed);
    EXPECT_EQ(journal.folder_ids(), (std::vector<std::int64_t>{folder.id}));
}

TEST_F(SessionOpenTest, WelcomeMaintenanceSkipsLibraryAndAttachUsesTheActiveVault) {
    const auto folder = store_->create_note_item({}, NoteItemKind::folder, "Research");
    auto prepared = sessions_->prepare({std::string(entrance_id), std::string(welcome_id)});
    std::vector<GenerationRequest> requests;
    bool reject = false;
    Providers providers([&](auto) { return std::make_unique<LibraryRequestBackend>(requests, reject); }, [](auto work) { work(); });
    int lookups = 0;
    auto controller = SessionController::from_workspace([&] { return store_->snapshot(); },
        [&](auto id) { return store_->get_note_text(id, true); },
        [&](auto id) { ++lookups; return store_->get_note_item(id); },
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
    EXPECT_TRUE(requests[0].library_store_ids.empty());
}


TEST_F(SessionOpenTest, ProductionControllersRequireExplicitActiveVaultReaders) {
    const auto stored = sessions_->create("lobby", "Required readers");
    const auto prepared = sessions_->prepare(stored.identity);
    for (const bool missing_item : {false, true}) {
        SessionController::ItemReader read_item = [&](auto id) { return store_->get_note_item(id); };
        SessionController::NoteReader read_note = [&](auto id) { return store_->get_note_text(id, true); };
        if (missing_item) read_item = {};
        else read_note = {};
        EXPECT_THROW((void)SessionController::from_workspace([&] { return store_->snapshot(); },
            read_note, read_item,
            "guide", std::string(workspace_guest_id), prepared.database_path, prepared.session_key,
            prepared.database_password, providers_, std::make_shared<test::TestNotifier>(),
            prepared.restore, prepared.identity), std::invalid_argument);
    }
}

} // namespace
} // namespace cha
