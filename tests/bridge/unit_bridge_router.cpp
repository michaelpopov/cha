#include "app/application.h"
#include "bridge/bridge_router.h"
#include "storage/note_storage.h"
#include "storage/session_lease.h"
#include "storage/sqlite_storage.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "runtime/command_queue.h"
#include "workspace/builtins.h"
#include "workspace/workspace_config_store.h"
#include "util/base64.h"
#include "util/path_name.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

namespace cha::bridge {
namespace {

using namespace std::chrono_literals;

std::string etag_response(std::string_view etag) {
    return "HTTP/1.1 200 OK\r\nETag: \"" + std::string(etag)
        + "\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

ApplicationCommand make_command(
    const test::TestWorkspace& workspace,
    const std::filesystem::path& database) {
    const std::filesystem::path config_directory =
        workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(database.string()) << "\n";
    }
    const ConfigurationDirectory loaded = load_configuration_directory(
        config_directory);
    const VaultDefinition* const vault =
        find_vault(loaded.vaults, loaded.startup_vault);
    std::vector<VaultDefinition> vaults = loaded.vaults;
    VaultDefinition selected = *vault;
    vaults.front() = selected;
    return {
        .config_directory = loaded.directory,
        .vaults = std::move(vaults),
        .vault = std::move(selected),
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

nlohmann::json request_json(
    std::string_view connection,
    std::uint64_t id,
    std::uint64_t epoch,
    std::string_view method,
    nlohmann::json params = nlohmann::json::object()) {
    return {
        {"connection_id", connection},
        {"id", id},
        {"context_epoch", epoch},
        {"method", method},
        {"params", std::move(params)},
    };
}

std::optional<nlohmann::json> wait_delivery(
    BridgeRouter& router,
    const std::string& connection,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        router.run_tasks();
        router.pump_output();
        if (auto batch = router.take_delivery(connection)) return batch;
        router.wait_for_work(5ms);
    }
    return std::nullopt;
}

void ack_delivery(
    BridgeRouter& router,
    const std::string& connection,
    const nlohmann::json& batch) {
    router.handle_ack(
        connection,
        nlohmann::json{
            {"connection_id", connection},
            {"delivery_id", batch.at("delivery_id")},
        }.dump());
}

nlohmann::json reply_with_id(const nlohmann::json& batch, std::uint64_t id) {
    for (const auto& message : batch.at("messages")) {
        if (message.contains("id") && message["id"] == id) return message;
    }
    return {};
}

class BridgeRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        database_ = test::import_test_database(workspace_.root());
        application_ = app::Application::open(make_command(workspace_, database_));
        router_ = std::make_unique<BridgeRouter>(*application_);
        connection_ = router_->open_connection();
    }

    void TearDown() override {
        if (router_) router_->shutdown();
        if (application_) {
            application_->request_shutdown();
            (void)application_->join_shutdown(2s);
        }
    }

    std::uint64_t bootstrap_epoch() {
        router_->handle_request(
            connection_,
            request_json(connection_, next_id_++, 0, "app.bootstrap").dump());
        const auto batch = wait_delivery(*router_, connection_);
        EXPECT_TRUE(batch);
        ack_delivery(*router_, connection_, *batch);
        const auto reply = reply_with_id(*batch, next_id_ - 1);
        EXPECT_TRUE(reply["ok"]);
        epoch_ = reply["result"]["context_epoch"].get<std::uint64_t>();
        return epoch_;
    }

    nlohmann::json call(
        std::string_view method,
        nlohmann::json params = nlohmann::json::object()) {
        const auto id = next_id_++;
        router_->handle_request(
            connection_,
            request_json(connection_, id, epoch_, method, std::move(params)).dump());
        const auto batch = wait_delivery(*router_, connection_);
        EXPECT_TRUE(batch) << method;
        ack_delivery(*router_, connection_, *batch);
        return reply_with_id(*batch, id);
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::unique_ptr<app::Application> application_;
    std::unique_ptr<BridgeRouter> router_;
    std::string connection_;
    std::uint64_t epoch_{1};
    std::uint64_t next_id_{1};
};

TEST_F(BridgeRouterTest, EventsListUsesEmptyParametersAndReturnsOnlyStoredRecords) {
    bootstrap_epoch();
    EXPECT_EQ(call("events.list")["result"], (nlohmann::json{{"items", nlohmann::json::array()}}));
    application_->record_event(EventSeverity::warning, "Check the connection.", epoch_);
    const auto reply = call("events.list");
    ASSERT_TRUE(reply["ok"]);
    ASSERT_EQ(reply["result"]["items"].size(), 1U);
    const auto item = reply["result"]["items"][0];
    EXPECT_EQ(item.size(), 4U);
    EXPECT_GT(item["id"].get<std::int64_t>(), 0);
    EXPECT_GE(item["recorded_at"].get<std::int64_t>(), 0);
    EXPECT_EQ(item["severity"], "warning");
    EXPECT_EQ(item["message"], "Check the connection.");
    EXPECT_FALSE(call("events.list", {{"unused", true}})["ok"]);
    storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
    database.execute("DROP TABLE event_history");
    const auto failed = call("events.list");
    EXPECT_FALSE(failed["ok"]);
    EXPECT_EQ(failed["error"]["code"], "internal_error");
    EXPECT_EQ(failed["error"]["message"], "Could not load events");
}

TEST_F(BridgeRouterTest, NoteTextFileReadingUsesFullPathsAndValidatesTextWithoutChangingNotes) {
    bootstrap_epoch();
    const auto path = workspace_.root() / path_from_utf8("Plan с пробелами.md");
    const auto read = [&] { return call("notes.readTextFile", {{"path", utf8_path(path)}}); };
    std::ofstream(path, std::ios::binary) << "\xef\xbb\xbf# Title\n\nBody";
    const auto reply = read();
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(reply["result"], (nlohmann::json{{"name", utf8_path(path)}, {"text", "# Title\n\nBody"}}));
    EXPECT_TRUE(call("notes.list")["result"]["items"].empty());
    for (const auto& [text, message] : std::vector<std::pair<std::string, std::string>>{
            {" \n\t", "The file is empty."},
            {std::string("a\0b", 3), "Choose a UTF-8 text file."},
            {"\xff\xfe", "Choose a UTF-8 text file."},
            {std::string(256 * 1024 + 1, 'x'),
                "This file is too large to load into a note. The limit is 256 KiB of text."}}) {
        std::ofstream file(path, std::ios::binary);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();
        const auto invalid = read();
        EXPECT_FALSE(invalid["ok"]) << invalid;
        EXPECT_EQ(invalid["error"]["message"], message);
    }
    const auto directory = call("notes.readTextFile", {{"path", utf8_path(workspace_.root())}});
    EXPECT_FALSE(directory["ok"]);
    EXPECT_EQ(directory["error"]["message"], "The path does not refer to a regular file.");
    const auto missing = call("notes.readTextFile", {{"path", utf8_path(workspace_.root() / "missing.md")}});
    EXPECT_FALSE(missing["ok"]);
    EXPECT_EQ(missing["error"]["message"], "That file was not found.");
    const auto prompt = call("notes.readTextFile", {{"path", utf8_path(workspace_.root() / "build.log fails, why?")}});
    ASSERT_TRUE(prompt["ok"]) << prompt;
    EXPECT_TRUE(prompt["result"].is_null());
    for (const auto& params : std::vector<nlohmann::json>{
            nlohmann::json::object(), {{"path", "relative.md"}}, {{"path", 3}},
            {{"path", utf8_path(path)}, {"extra", true}}, {{"path", utf8_path(path) + "\ntext"}},
            {{"path", utf8_path(path) + std::string(1, '\0')}}}) {
        EXPECT_FALSE(call("notes.readTextFile", params)["ok"]);
    }
    ++epoch_;
    EXPECT_EQ(read()["error"]["code"], "vault_changed");
}

TEST_F(BridgeRouterTest, NotePageReadingUsesConfiguredReaderAndKeepsLongArticles) {
    bootstrap_epoch();
    std::string article = "A **formatted** article.";
    for (int i = 0; i < 80; ++i) article += "\n\n" + std::string(1000, 'x');
    const nlohmann::json page{{"url", "https://example.org/final"}, {"title", "Article title"},
        {"markdown", article + "\n\n![Photo](https://example.org/photo.png)"}};
    MockHttpServer server({http_response("application/json", page.dump())});
    server.start();
    auto settings = call("webSearch.get")["result"];
    settings["web_reader_enabled"] = true;
    settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(server.port());
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    const auto reply = call("notes.readPage", {{"url", "https://example.org/original"}});
    server.join();
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(reply["result"]["url"], page["url"]);
    EXPECT_EQ(reply["result"]["title"], page["title"]);
    EXPECT_EQ(reply["result"]["markdown"], page["markdown"]);
    EXPECT_EQ(reply["result"].size(), 3u);
    EXPECT_TRUE(call("notes.list")["result"]["items"].empty());
    ASSERT_EQ(server.requests().size(), 1u);
    EXPECT_TRUE(server.requests()[0].starts_with("POST /extract HTTP/1.1\r\n"));
}

TEST_F(BridgeRouterTest, NotePictureReadingUsesBinaryDownloadWithoutReaderConfiguration) {
    bootstrap_epoch();
    const std::string bytes("\x89PNG\r\n\x1a\n\0", 9);
    MockHttpServer server({http_response("image/png", bytes)});
    server.start();
    const auto reply = call("notes.readPicture", {{"url",
        "http://127.0.0.1:" + std::to_string(server.port()) + "/picture.png"}});
    server.join();
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(reply["result"], (nlohmann::json{{"base64", "iVBORw0KGgoA"}}));
    EXPECT_FALSE(call("notes.readPicture", nlohmann::json::object())["ok"]);
    EXPECT_FALSE(call("notes.readPicture", {{"url", "file:///private/file"}})["ok"]);
}

TEST_F(BridgeRouterTest, NotePageReadingReportsConfigurationAndFetchFailures) {
    bootstrap_epoch();
    for (const auto& params : std::vector<nlohmann::json>{
            nlohmann::json::object(), {{"url", 42}},
            {{"url", "https://example.org"}, {"extra", true}}}) {
        EXPECT_FALSE(call("notes.readPage", params)["ok"]);
    }
    auto reply = call("notes.readPage", {{"url", "https://example.org"}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    EXPECT_NE(reply["error"]["message"].get<std::string>().find("not configured"), std::string::npos);

    MockHttpServer server({"HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    server.start();
    auto settings = call("webSearch.get")["result"];
    settings["web_reader_enabled"] = true;
    settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(server.port());
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    reply = call("notes.readPage", {{"url", "https://example.org"}});
    server.join();
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["message"], "Page reading HTTP 503");
    reply = call("notes.readPage", {{"url", "file:///private/file"}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["message"], "Page reading supports only HTTP and HTTPS URLs");
}

TEST_F(BridgeRouterTest, ForumSystemPromptIsReadOnlyAndRequiresExactIdentifiers) {
    bootstrap_epoch();
    const auto params = nlohmann::json{{"forum_id", "lobby"}, {"character_id", "guide"}};
    const auto before = call("forum.get", {{"forum_id", "lobby"}});
    const auto reply = call("forum.systemPrompt", params);
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_NE(reply["result"]["system_prompt"].get<std::string>().find("Character instructions"), std::string::npos);
    EXPECT_EQ(call("forum.get", {{"forum_id", "lobby"}})["result"], before["result"]);
    EXPECT_FALSE(call("forum.systemPrompt", {{"forum_id", "lobby"}})["ok"]);
    auto extra = params;
    extra["persona_id"] = "reader";
    EXPECT_FALSE(call("forum.systemPrompt", extra)["ok"]);
    EXPECT_EQ(call("forum.systemPrompt", {{"forum_id", "missing"}, {"character_id", "guide"}})
        ["error"]["code"], "not_found");
}

TEST_F(BridgeRouterTest, CharacterInstructionsUseFixedVaultPathAndHaveNoDeleteMethod) {
    bootstrap_epoch();
    auto reply = call("character.instructions.get");
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["filename"], "character-voice.md");
    reply = call("character.instructions.update", {{"content", "Shared voice."}});
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(reply["result"]["content"], "Shared voice.");
    EXPECT_EQ(call("character.instructions.get")["result"]["content"], "Shared voice.");
    for (const auto& params : std::vector<nlohmann::json>{
             {}, {{"content", 42}}, {{"content", "Wrong"}, {"filename", "elsewhere.md"}}}) {
        EXPECT_FALSE(call("character.instructions.update", params)["ok"]);
    }
    EXPECT_FALSE(call("character.instructions.get", {{"character_id", "guide"}})["ok"]);
    EXPECT_FALSE(call("character.instructions.delete")["ok"]);
    EXPECT_EQ(call("character.instructions.get")["result"]["content"], "Shared voice.");
}

TEST_F(BridgeRouterTest, CharacterPictureReadsNullAndStoredContentWithExactParams) {
    router_->shutdown();
    application_->request_shutdown();
    ASSERT_TRUE(application_->join_shutdown(2s));
    router_.reset();
    application_.reset();
    auto command = make_command(workspace_, database_);
    const auto modify = workspace_.root() / "modify";
    command.vault.modify = modify;
    command.vaults.front().modify = modify;
    application_ = app::Application::open(std::move(command));
    router_ = std::make_unique<BridgeRouter>(*application_);
    connection_ = router_->open_connection();
    bootstrap_epoch();
    auto reply = call("character.picture.get", {{"character_id", "guide"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_TRUE(reply["result"].is_null());
    EXPECT_EQ(reply["context_epoch"], epoch_);
    reply = call("character.picture.get", {{"character_id", "missing"}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "not_found");
    for (const auto& params : std::vector<nlohmann::json>{
             {}, {{"character_id", ""}}, {{"character_id", 1}},
             {{"character_id", "../guide"}},
             {{"character_id", "guide"}, {"path", "PICTURE.png"}},
             {{"character_id", "guide"}, {"filename", "OTHER.svg"}},
             {{"character_id", "guide"}, {"filename", "../PICTURE.png"}}}) {
        reply = call("character.picture.get", params);
        EXPECT_FALSE(reply["ok"]) << params;
        EXPECT_EQ(reply["error"]["code"], "invalid_argument") << params;
    }
    const auto old_epoch = epoch_;
    reply = call("vault.export");
    ASSERT_TRUE(reply["ok"]) << reply;
    epoch_ = reply["result"]["context_epoch"].get<std::uint64_t>();
    std::ofstream(modify / "characters/guide/PICTURE.png")
        << "not an image";
    reply = call("vault.import");
    ASSERT_TRUE(reply["ok"]) << reply;
    epoch_ = reply["result"]["context_epoch"].get<std::uint64_t>();
    reply = call("character.picture.get", {{"character_id", "guide"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"], nlohmann::json({
        {"filename", "PICTURE.png"}, {"mime_type", "image/png"},
        {"content_base64", "bm90IGFuIGltYWdl"}}));
    reply = call("character.picture.get", {{"character_id", "guide"}, {"filename", "PICTURE.png"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["content_base64"], "bm90IGFuIGltYWdl");
    reply = call("character.file.delete", {{"character_id", "guide"}, {"filename", "PICTURE.png"}});
    ASSERT_TRUE(reply["ok"]) << reply;
    reply = call("character.picture.get", {{"character_id", "guide"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_TRUE(reply["result"].is_null());
    epoch_ = old_epoch;
    reply = call("character.picture.get", {{"character_id", "guide"}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "vault_changed");
}

TEST_F(BridgeRouterTest, CharacterFileRenameUpdatesListsAndPreservesTextAndPictureContent) {
    bootstrap_epoch();
    ASSERT_TRUE(call("character.file.create", {{"character_id", "guide"}, {"filename", "NOTES.txt"}, {"content", "Original text"}})["ok"]);
    auto reply = call("character.file.rename", {{"character_id", "guide"}, {"filename", "NOTES.txt"}, {"new_filename", "RENAMED.txt"}});
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(call("character.file.get", {{"character_id", "guide"}, {"filename", "RENAMED.txt"}})["result"]["content"], "Original text");
    EXPECT_FALSE(call("character.file.get", {{"character_id", "guide"}, {"filename", "NOTES.txt"}})["ok"]);
    ASSERT_TRUE(call("character.picture.update", {{"character_id", "guide"}, {"filename", "portrait.png"}, {"content_base64", "AP+A"}})["ok"]);
    reply = call("character.file.rename", {{"character_id", "guide"}, {"filename", "portrait.png"}, {"new_filename", "Seneca.png"}});
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(call("character.picture.get", {{"character_id", "guide"}, {"filename", "Seneca.png"}})["result"]["content_base64"], "AP+A");
    const auto files = call("character.get", {{"character_id", "guide"}})["result"]["markdown_files"].get<std::vector<std::string>>();
    EXPECT_NE(std::find(files.begin(), files.end(), "Seneca.png"), files.end());
    EXPECT_EQ(std::find(files.begin(), files.end(), "portrait.png"), files.end());
    for (const auto& params : std::vector<nlohmann::json>{
             {{"character_id", "guide"}, {"filename", "Seneca.png"}},
             {{"character_id", "guide"}, {"filename", "Seneca.png"}, {"new_filename", "../elsewhere.png"}},
             {{"character_id", "guide"}, {"filename", "Seneca.png"}, {"new_filename", "CHARACTER.md"}},
             {{"character_id", "guide"}, {"filename", "Seneca.png"}, {"new_filename", "other.png"}, {"extra", true}}}) {
        reply = call("character.file.rename", params);
        EXPECT_FALSE(reply["ok"]) << params;
        EXPECT_EQ(reply["error"]["code"], "invalid_argument") << reply;
    }
    ++epoch_;
    reply = call("character.file.rename", {{"character_id", "guide"}, {"filename", "Seneca.png"}, {"new_filename", "other.png"}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "vault_changed");
}

TEST_F(BridgeRouterTest, PictureUploadsAcceptLargeFilesAndValidateParamsAndContext) {
    bootstrap_epoch();
    const auto content = encode_base64(std::string(2 * 1024 * 1024, '\xff'));
    const nlohmann::json params{
        {"character_id", "guide"}, {"filename", "PICTURE.jpeg"}, {"content_base64", content}};
    auto reply = call("character.picture.update", params);
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(reply["context_epoch"], epoch_);
    reply = call("character.picture.get", {{"character_id", "guide"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["filename"], "PICTURE.jpeg");
    EXPECT_EQ(reply["result"]["content_base64"], content);
    for (const auto& invalid : std::vector<nlohmann::json>{
             {}, {{"character_id", "guide"}, {"filename", "PICTURE.jpeg"}},
             {{"character_id", "guide"}, {"filename", "PICTURE.png"}, {"content_base64", 42}},
             {{"character_id", "guide"}, {"filename", "../PICTURE.png"}, {"content_base64", "AP8="}},
             {{"character_id", "guide"}, {"filename", "PICTURE.svg"}, {"content_base64", "AP8="}},
             {{"character_id", "guide"}, {"filename", "PICTURE.png"}, {"content_base64", "!"}},
             {{"character_id", "guide"}, {"filename", "PICTURE.png"}, {"content_base64", "AP8="}, {"extra", true}}}) {
        reply = call("character.picture.update", invalid);
        EXPECT_FALSE(reply["ok"]) << invalid;
        EXPECT_EQ(reply["error"]["code"], "invalid_argument") << invalid;
    }
    auto missing = params;
    missing["character_id"] = "missing";
    reply = call("character.picture.update", missing);
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "not_found");
    ++epoch_;
    reply = call("character.picture.update", params);
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "vault_changed");
    --epoch_;
    EXPECT_EQ(call("character.picture.get", {{"character_id", "guide"}})["result"]["content_base64"], content);
    reply = call("character.picture.update", {{"character_id", "guide"},
        {"filename", "portrait.png"}, {"content_base64", "bmV3"}, {"previous_filename", "PICTURE.jpeg"}});
    ASSERT_TRUE(reply["ok"]) << reply;
    EXPECT_EQ(call("character.picture.get", {{"character_id", "guide"}, {"filename", "portrait.png"}})["result"]["content_base64"], "bmV3");
    EXPECT_TRUE(call("character.picture.get", {{"character_id", "guide"}, {"filename", "PICTURE.jpeg"}})["result"].is_null());
}

TEST_F(BridgeRouterTest, BackgroundAudioCommandsValidateAndStatusReadsTheDurableRow) {
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Chapter", epoch_);
    (void)application_->set_note_read_only(note.id, true, epoch_);
    EXPECT_EQ(call("noteAudio.backgroundStatus", {{"id", note.id}})["result"], (nlohmann::json{{"pending", false}}));
    EXPECT_EQ(call("noteAudio.backgroundStatus", {{"id", 0}})["error"]["code"], "invalid_argument");
    EXPECT_EQ(call("noteAudio.backgroundStatus", {{"id", note.id}, {"extra", true}})["error"]["code"], "invalid_argument");
    const auto attempt = *application_->store().accept_note_audio(note.id, 1, "http://localhost:1", "key-1", [](const auto&) {});
    EXPECT_EQ(call("noteAudio.backgroundStatus", {{"id", note.id}})["result"],
        (nlohmann::json{{"pending", true}, {"attempt_id", attempt.id}}));
    const auto other = application_->create_note_item({}, NoteItemKind::note, "Other", epoch_);
    (void)application_->set_note_read_only(other.id, true, epoch_);
    const auto second = *application_->store().accept_note_audio(other.id, 1, "http://localhost:1", "key-1", [](const auto&) {});
    EXPECT_EQ(call("noteAudio.backgroundStatus", {{"id", other.id}})["result"],
        (nlohmann::json{{"pending", true}, {"attempt_id", second.id}}));
#if defined(__APPLE__) || defined(_WIN32)
    EXPECT_EQ(call("noteAudio.startBackground", {{"id", note.id}, {"voice", "Kore"}, {"blocks", nlohmann::json::array()}, {"only_if_missing", true}})["result"],
        (nlohmann::json{{"skipped", true}}));
#endif
    EXPECT_EQ(call("noteAudio.startBackground", {{"id", note.id}, {"voice", "Kore"}, {"blocks", {"Text"}}, {"only_if_missing", 1}})["error"]["code"], "invalid_argument");
    const auto blocked = call("noteAudio.startBackground", {{"id", note.id}, {"voice", "Kore"}, {"blocks", {"Text"}}});
    EXPECT_EQ(blocked["error"]["code"], "invalid_argument");
#if defined(__APPLE__) || defined(_WIN32)
    EXPECT_NE(blocked["error"]["message"].get<std::string>().find("Make that note editable"), std::string::npos);
#endif
    for (const auto& params : std::vector<nlohmann::json>{
        {{"id", 0}, {"voice", "Kore"}, {"blocks", {"Text"}}},
        {{"id", note.id}, {"voice", "Kore"}, {"blocks", 1}},
        {{"id", note.id}, {"voice", "Kore"}, {"blocks", {1}}},
        {{"id", note.id}, {"voice", "Kore"}, {"blocks", {"Text"}}, {"extra", true}}})
        EXPECT_EQ(call("noteAudio.startBackground", params)["error"]["code"], "invalid_argument");
}

#if defined(__APPLE__) || defined(_WIN32)
TEST_F(BridgeRouterTest, BackgroundCompletionPrecedesNewSourcesAndWaitsForDeliveryAcknowledgment) {
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Chapter", epoch_);
    (void)application_->set_note_read_only(note.id, true, epoch_);
    const auto old = workspace_.root() / "old.m4a";
    std::ofstream(old) << "old audio";
    application_->store().save_note_audio(note.id, {old});
    std::string wav;
    const auto number = [&](unsigned value, int bytes) {
        for (int i = 0; i < bytes; ++i) wav += static_cast<char>((value >> (8 * i)) & 255);
    };
    wav += "RIFF"; number(4836, 4); wav += "WAVEfmt "; number(16, 4);
    number(1, 2); number(1, 2); number(24000, 4); number(48000, 4); number(2, 2); number(16, 2);
    wav += "data"; number(4800, 4); wav.append(4800, '\0');
    const auto result = nlohmann::json{{"key", "0"}, {"response", {{"candidates", nlohmann::json::array({
        {{"content", {{"parts", nlohmann::json::array({{{"inlineData", {{"mimeType", "audio/wav"}, {"data", encode_base64(wav)}}}}})}}}}})}}}};
    const auto batch_id = "batches/" + std::string(32, 'b');
    MockHttpServer server({http_response("application/json", nlohmann::json{{"name", batch_id}, {"done", true},
        {"response", {{"responsesFile", "files/" + std::string(32, 'a')}}}}.dump()),
        http_response("application/jsonl", result.dump() + '\n')});
    server.pause_before_response(2);
    const auto key = application_->create_api_key({.display_name = "Audio", .value = "secret"}, epoch_);
    const auto attempt = *application_->store().accept_note_audio(note.id, 1,
        "http://127.0.0.1:" + std::to_string(server.port()), key.id, [](const auto&) {});
    ASSERT_TRUE(application_->store().register_note_audio(attempt.id, batch_id, "Kore"));
    const auto other = application_->create_note_item({}, NoteItemKind::note, "Other", epoch_);
    (void)application_->set_note_read_only(other.id, true, epoch_);
    application_->store().save_note_audio(other.id, {old});
    const auto other_batch = "batches/" + std::string(32, 'c');
    MockHttpServer other_server({http_response("application/json", nlohmann::json{{"name", other_batch}, {"done", true},
        {"response", {{"responsesFile", "files/" + std::string(32, 'd')}}}}.dump()),
        http_response("application/jsonl", result.dump() + '\n')});
    other_server.pause_before_response(2);
    const auto other_attempt = *application_->store().accept_note_audio(other.id, 1,
        "http://127.0.0.1:" + std::to_string(other_server.port()), key.id, [](const auto&) {});
    ASSERT_TRUE(application_->store().register_note_audio(other_attempt.id, other_batch, "Kore"));
    // Reinstalling the native router activates recovery without a webview command.
    router_.reset();
    server.start(); other_server.start();
    router_ = std::make_unique<BridgeRouter>(*application_);
    connection_ = router_->open_connection(); next_id_ = 1;
    bootstrap_epoch();
    ASSERT_TRUE(server.wait_for_requests(2, 2s));
    ASSERT_TRUE(other_server.wait_for_requests(2, 2s));
    const auto old_request = next_id_++;
    router_->handle_request(connection_, request_json(connection_, old_request, epoch_, "noteAudio.source",
        {{"id", note.id}, {"ordinal", 0}}).dump());
    const auto before = wait_delivery(*router_, connection_);
    ASSERT_TRUE(before);
    EXPECT_EQ(reply_with_id(*before, old_request)["result"]["byte_length"], 9);
    server.resume_responses(); other_server.resume_responses(); server.join(); other_server.join();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!application_->store().get_note_audio_attempts().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(application_->store().get_note_audio_attempts().empty());
    // Completion cannot overtake the old source's unacknowledged delivery.
    EXPECT_FALSE(router_->take_delivery(connection_));
    const auto new_request = next_id_++;
    router_->handle_request(connection_, request_json(connection_, new_request, epoch_, "noteAudio.source",
        {{"id", note.id}, {"ordinal", 0}}).dump());
    const auto other_request = next_id_++;
    router_->handle_request(connection_, request_json(connection_, other_request, epoch_, "noteAudio.source",
        {{"id", other.id}, {"ordinal", 0}}).dump());
    router_->run_tasks();
    ack_delivery(*router_, connection_, *before);
    const auto after = wait_delivery(*router_, connection_);
    ASSERT_TRUE(after);
    const auto& notice = after->at("messages")[0];
    EXPECT_EQ(notice, (nlohmann::json{{"connection_id", connection_}, {"event", "noteAudio.changed"},
        {"id", note.id}, {"context_epoch", epoch_}}));
    EXPECT_EQ(after->at("messages")[1].at("event"), "noteAudio.changed");
    EXPECT_EQ(after->at("messages")[1].at("id"), other.id);
    EXPECT_GT(reply_with_id(*after, other_request)["result"]["byte_length"].get<int>(), 9);
    EXPECT_GT(reply_with_id(*after, new_request)["result"]["byte_length"].get<int>(), 9);
    ack_delivery(*router_, connection_, *after);
    EXPECT_FALSE(router_->take_delivery(connection_));
}
#endif

TEST_F(BridgeRouterTest, NoteAudioValidatesBlockPayloadAndKeepsOfflineLookupIndependentOfSettings) {
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Narration", epoch_);
    (void)application_->set_note_read_only(note.id, true, epoch_);
    EXPECT_EQ(call("noteAudio.info", {{"id", note.id}})["result"], (nlohmann::json{{"count", 0}}));
    for (const auto& params : std::vector<nlohmann::json>{
        {{"id", note.id}, {"voice", "Kore"}},
        {{"id", note.id}, {"voice", "Kore"}, {"blocks", "text"}},
        {{"id", note.id}, {"voice", "Kore"}, {"blocks", nlohmann::json::array({42})}},
        {{"id", note.id}, {"voice", "Kore"}, {"blocks", nlohmann::json::array()}, {"extra", true}}}) {
        const auto reply = call("noteAudio.startBackground", params);
        EXPECT_FALSE(reply["ok"].get<bool>());
        EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    }
#ifdef __APPLE__
    const auto empty = call("noteAudio.startBackground", {{"id", note.id}, {"voice", "Kore"}, {"blocks", nlohmann::json::array()}});
    EXPECT_EQ(empty["error"]["code"], "invalid_argument");
#endif
    EXPECT_EQ(call("noteAudio.source", {{"id", note.id}, {"ordinal", 0}})["error"]["code"], "not_found");
}

TEST_F(BridgeRouterTest, AttachesTextFilesWithoutCreatingNotesOrClearingThePrompt) {
    bootstrap_epoch();
    const auto created = call("session.create", {{"forum_id", "lobby"}, {"label", "Files"}});
    ASSERT_TRUE(created["ok"]) << created;
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(call("session.open", {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);
    const nlohmann::json file{{"forum_id", "lobby"}, {"session_id", session_id},
        {"name", "plan.md"}, {"text", "# Plan\n\nFile context"}};
    const auto attached = call("session.attachTextFile", file);
    ASSERT_TRUE(attached["ok"]) << attached;
    EXPECT_FALSE(attached["result"]["clear_input"]);
    const auto snapshot = call("session.snapshot", {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(snapshot["ok"]) << snapshot;
    EXPECT_FALSE(snapshot["result"]["generation"]["active"]);
    EXPECT_EQ(snapshot["result"]["transcript"].back()["text"], "Added file “plan.md” to this chat.");
    EXPECT_TRUE(call("notes.list")["result"]["items"].empty());
    auto malformed = file;
    malformed["text"] = 42;
    EXPECT_EQ(call("session.attachTextFile", malformed)["error"]["code"], "invalid_argument");
    auto oversized = file;
    oversized["text"] = std::string(256 * 1024 + 1, 'x');
    const auto rejected = call("session.attachTextFile", oversized);
    ASSERT_TRUE(rejected["ok"]) << rejected;
    EXPECT_EQ(rejected["result"]["notice"],
        "This file is too large to add to a chat. The limit is 256 KiB of text.");
    ++epoch_;
    EXPECT_EQ(call("session.attachTextFile", file)["error"]["code"], "vault_changed");
}

TEST_F(BridgeRouterTest, SubmitsAbsoluteTextFilePathsAsAttachmentsAndKeepsMissingPathsInTheEditor) {
    bootstrap_epoch();
    const auto created = call("session.create", {{"forum_id", "lobby"}, {"label", "Files"}});
    ASSERT_TRUE(created["ok"]) << created;
    const std::string session_id = created["result"]["id"];
    const nlohmann::json identity{{"forum_id", "lobby"}, {"session_id", session_id}};
    ASSERT_TRUE(call("session.open", identity)["ok"]);
    const auto path = workspace_.root() / "plan with spaces.md";
    std::ofstream(path, std::ios::binary) << "# Plan\n\nFile context";
    auto submit = identity;
    submit["input"] = {{"text", utf8_path(path)}};
    const auto attached = call("session.submit", submit);
    ASSERT_TRUE(attached["ok"]) << attached;
    EXPECT_TRUE(attached["result"]["clear_input"]);
    const auto snapshot = call("session.snapshot", identity);
    ASSERT_TRUE(snapshot["ok"]) << snapshot;
    EXPECT_FALSE(snapshot["result"]["generation"]["active"]);
    ASSERT_EQ(snapshot["result"]["transcript"].size(), 1U);
    EXPECT_EQ(snapshot["result"]["transcript"].back()["text"],
        "Added file “" + utf8_path(path) + "” to this chat.");
    std::filesystem::remove(path);
    submit["input"] = {{"text", utf8_path(workspace_.root() / "missing.md")}};
    const auto missing = call("session.submit", submit);
    ASSERT_TRUE(missing["ok"]) << missing;
    EXPECT_FALSE(missing["result"]["clear_input"]);
    EXPECT_EQ(missing["result"]["notice"], "That file was not found.");
}

TEST_F(BridgeRouterTest, InfoBootstrapCreateOpenSubmitStopSnapshotAndClose) {
    router_->handle_request(
        connection_,
        request_json(connection_, next_id_++, 0, "bridge.info").dump());
    auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    auto reply = reply_with_id(*batch, 1);
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["protocol_version"], kProtocolVersion);
    EXPECT_EQ(reply["result"]["platform"], "test");
    ack_delivery(*router_, connection_, *batch);

    bootstrap_epoch();
    reply = call("session.create", {{"forum_id", "lobby"}, {"label", "Bridge"}});
    ASSERT_TRUE(reply["ok"]);
    const std::string session_id = reply["result"]["id"];

    reply = call(
        "session.open", {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["session_id"], session_id);

    reply = call(
        "session.submit",
        {{"forum_id", "lobby"},
         {"session_id", session_id},
         {"input", {{"text", "Hello"}}}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_TRUE(reply["result"].contains("clear_input"));

    reply = call(
        "session.stop", {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);

    reply = call(
        "session.snapshot",
        {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["session_id"], session_id);

    reply = call(
        "session.close", {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);
}

TEST_F(BridgeRouterTest, RecipientDetectionSettingsApplyToExistingSessionAndMissingKeyFallsBack) {
    bootstrap_epoch();
    EXPECT_TRUE(call("jev.get")["result"].is_null());
    const auto key = call("apiKey.create", {{"display_name", "OpenRouter"}, {"value", "test-secret"}});
    ASSERT_TRUE(key["ok"]);
    auto created = call("session.create", {{"forum_id", "lobby"}, {"label", "Jev"}});
    ASSERT_TRUE(created["ok"]);
    const auto session_id = created["result"]["id"];
    ASSERT_TRUE(call("session.open", {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);
    const std::string body = R"({"answers":{"recipient":{"type":"choice","choice":"all_characters"},"web_search":{"type":"choice","choice":"no_search"}}})";
    MockHttpServer server({"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body});
    server.start();
    nlohmann::json settings{
        {"url", "http://127.0.0.1:" + std::to_string(server.port()) + "/api/alpha/decisions"},
        {"model", "typesafe/jev-1.13"}, {"api_key", key["result"]["id"]}};
    ASSERT_TRUE(call("jev.save", settings)["ok"]);
    EXPECT_EQ(call("jev.get")["result"], settings);
    auto invalid = settings; invalid["url"] = "relative";
    EXPECT_FALSE(call("jev.save", invalid)["ok"]);
    EXPECT_EQ(call("jev.get")["result"], settings);
    const auto submitted = call("session.submit", {{"forum_id", "lobby"}, {"session_id", session_id},
        {"input", {{"text", "Everyone, answer"}}}});
    ASSERT_TRUE(submitted["ok"]);
    EXPECT_TRUE(submitted["result"]["clear_input"]);
    server.join();
    EXPECT_EQ(server.requests().size(), 1u);
    auto snapshot = call("session.snapshot", {{"forum_id", "lobby"}, {"session_id", session_id}})["result"];
    EXPECT_EQ(snapshot["default_character_id"], "*");
    EXPECT_EQ(snapshot["forum"]["default_character_id"], "guide");
    ASSERT_TRUE(call("apiKey.delete", {{"api_key_id", key["result"]["id"]}})["ok"]);
    // Let the deterministic character worker finish before submitting again.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (snapshot["generation"]["active"].get<bool>() && std::chrono::steady_clock::now() < deadline)
        snapshot = call("session.snapshot", {{"forum_id", "lobby"}, {"session_id", session_id}})["result"];
    const auto fallback = call("session.submit", {{"forum_id", "lobby"}, {"session_id", session_id},
        {"input", {{"text", "Another question"}}}});
    ASSERT_TRUE(fallback["ok"]);
    EXPECT_TRUE(fallback["result"]["clear_input"]);
    EXPECT_EQ(fallback["result"]["notice"], "Recipient detection failed. Sent to all characters.");
    ASSERT_TRUE(call("jev.disable")["ok"]);
    EXPECT_TRUE(call("jev.get")["result"].is_null());
}

TEST_F(BridgeRouterTest, SavesSessionNamingSettings) {
    bootstrap_epoch();
    const auto initial = call("sessionNaming.get");
    ASSERT_TRUE(initial["ok"]);
    EXPECT_EQ(initial["result"]["reasoning_effort"], "low");
    const nlohmann::json settings{{"provider", "test"}, {"reasoning_effort", "high"}};
    ASSERT_TRUE(call("sessionNaming.save", settings)["ok"]);
    EXPECT_EQ(call("sessionNaming.get")["result"], settings);
    auto invalid = settings;
    invalid["provider"] = "missing";
    EXPECT_FALSE(call("sessionNaming.save", invalid)["ok"]);
    EXPECT_EQ(call("sessionNaming.get")["result"], settings);
}

TEST_F(BridgeRouterTest, SavesNotesSettingsAndIgnoresUnusedFields) {
    bootstrap_epoch();
    const auto initial = call("notesSettings.get");
    ASSERT_TRUE(initial["ok"]);
    EXPECT_EQ(initial["result"], nlohmann::json({{"compression_provider", ""}, {"compression_reasoning_effort", "high"},
        {"audio_url", ""}, {"audio_api_key", ""}}));
    const nlohmann::json settings{{"compression_provider", "test"}, {"compression_reasoning_effort", "medium"},
        {"audio_url", "http://192.168.1.10:18080"}, {"audio_api_key", ""}};
    auto obsolete = settings;
    obsolete["unused"] = true;
    ASSERT_TRUE(call("notesSettings.save", obsolete)["ok"]);
    EXPECT_EQ(call("notesSettings.get")["result"], settings);
    for (const auto& invalid : {
            nlohmann::json{{"compression_provider", "missing"}, {"compression_reasoning_effort", "high"}},
            nlohmann::json{{"compression_provider", 42}, {"compression_reasoning_effort", "high"}},
            nlohmann::json{{"compression_provider", "test"}, {"compression_reasoning_effort", "invalid"}},
            nlohmann::json{{"compression_provider", "test"}, {"compression_reasoning_effort", 42}},
            nlohmann::json{{"compression_provider", "test"}},
            nlohmann::json::object()}) {
        EXPECT_FALSE(call("notesSettings.save", invalid)["ok"]);
        EXPECT_EQ(call("notesSettings.get")["result"], settings);
    }
}

TEST_F(BridgeRouterTest, PageReadingSettingsRetainKeyAndAreIndependentOfSearch) {
    bootstrap_epoch();
    auto settings = call("webSearch.get")["result"];
    const auto firecrawl = call("apiKey.create", {{"display_name", "Firecrawl"}, {"value", "fc-secret"}});
    ASSERT_TRUE(firecrawl["ok"]);
    settings["read_provider"] = "firecrawl";
    EXPECT_FALSE(call("webSearch.save", settings)["ok"]);
    settings["firecrawl_api_key"] = firecrawl["result"]["id"];
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], settings);
    auto keys = call("apiKey.list")["result"];
    EXPECT_EQ(keys[0]["used_by"], nlohmann::json::array({"Page reading"}));
    settings["read_provider"] = "off";
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], settings);
    keys = call("apiKey.list")["result"];
    EXPECT_TRUE(keys[0]["used_by"].empty());
    settings["firecrawl_api_key"] = "obsolete-key";
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], settings);
    settings["read_provider"] = "obsolete-reader";
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"]["read_provider"], "off");
    // Older clients can still save search settings without reader fields.
    settings.erase("read_provider");
    settings.erase("firecrawl_api_key");
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"]["read_provider"], "off");
}

TEST_F(BridgeRouterTest, RemovedReaderSettingsAreIgnoredWithoutBlockingSearch) {
    bootstrap_epoch();
    const auto key = call("apiKey.create", {{"display_name", "Search"}, {"value", "search-secret"}});
    ASSERT_TRUE(key["ok"]);
    auto settings = call("webSearch.get")["result"];
    settings["tool_enabled"] = true;
    settings["api_key"] = key["result"]["id"];
    settings["read_provider"] = "jina";
    settings["jina_api_key"] = 123;
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    const auto saved = call("webSearch.get")["result"];
    EXPECT_TRUE(saved["tool_enabled"]);
    EXPECT_EQ(saved["api_key"], settings["api_key"]);
    EXPECT_EQ(saved["read_provider"], "off");
    EXPECT_FALSE(saved.contains("jina_api_key"));
}

TEST_F(BridgeRouterTest, WebReaderSettingsWorkWithoutKeysAndValidateTheBaseUrl) {
    bootstrap_epoch();
    auto settings = call("webSearch.get")["result"];
    settings["web_reader_enabled"] = true;
    settings["web_reader_url"] = "http://192.168.86.39:8087";
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], settings);
    for (const auto* url : {"relative", "ftp://host", "http://", "http://host:bad/",
            "http://host/a b", "http://user:password@host", "http://host?token=secret", "http://host#fragment"}) {
        auto invalid = settings;
        invalid["web_reader_url"] = url;
        EXPECT_FALSE(call("webSearch.save", invalid)["ok"]);
        EXPECT_EQ(call("webSearch.get")["result"], settings);
    }
    const auto firecrawl = call("apiKey.create", {{"display_name", "Firecrawl"}, {"value", "fc-secret"}});
    ASSERT_TRUE(firecrawl["ok"]);
    settings["firecrawl_api_key"] = firecrawl["result"]["id"];
    settings["read_provider"] = "firecrawl";
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("apiKey.list")["result"][0]["used_by"], nlohmann::json::array({"Page reading"}));
    const auto reader = call("apiKey.create", {{"display_name", "web_reader"}, {"value", "reader-secret"}});
    ASSERT_TRUE(reader["ok"]);
    settings["web_reader_api_key"] = reader["result"]["id"];
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], settings);
    const auto key_list = call("apiKey.list")["result"];
    for (const auto& key : key_list) {
        if (key["id"] == reader["result"]["id"])
            EXPECT_EQ(key["used_by"], nlohmann::json::array({"web_reader"}));
    }
    // This reserved key has no authentication role in the service yet.
    settings["web_reader_api_key"] = "obsolete-reader-key";
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    settings["read_provider"] = "off";
    settings["web_reader_enabled"] = false;
    settings["web_reader_url"] = "ftp://obsolete";
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    settings["web_reader_url"] = "";
    EXPECT_EQ(call("webSearch.get")["result"], settings);
    settings.erase("web_reader_url");
    settings.erase("web_reader_enabled");
    settings.erase("web_reader_api_key");
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"]["web_reader_url"], "");
    EXPECT_FALSE(call("webSearch.get")["result"]["web_reader_enabled"].get<bool>());
    EXPECT_EQ(call("webSearch.get")["result"]["web_reader_api_key"], "");
}

TEST_F(BridgeRouterTest, WebSearchSettingsRoundTripAndIgnoreObsoletePreliminarySearchSettings) {
    bootstrap_epoch();
    const nlohmann::json disabled = {
        {"provider", "brave"}, {"api_key", ""}, {"tool_enabled", false},
        {"read_provider", "off"}, {"firecrawl_api_key", ""}, {"web_reader_url", ""}, {"web_reader_enabled", false}, {"web_reader_api_key", ""}};
    EXPECT_EQ(call("webSearch.get")["result"], disabled);
    auto obsolete = disabled;
    obsolete["enabled"] = true;
    obsolete["query_provider"] = 123;
    ASSERT_TRUE(call("webSearch.save", obsolete)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], disabled);
    obsolete["provider"] = "google";
    ASSERT_TRUE(call("webSearch.save", obsolete)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], disabled);

    auto enabled = disabled;
    enabled["tool_enabled"] = true;
    EXPECT_FALSE(call("webSearch.save", enabled)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], disabled);
    const auto key = call("apiKey.create", {
        {"display_name", "Search"}, {"value", "test-secret"}});
    ASSERT_TRUE(key["ok"]);
    enabled["api_key"] = key["result"]["id"];
    enabled["provider"] = "tavily";
    ASSERT_TRUE(call("webSearch.save", enabled)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], enabled);
    auto invalid = enabled;
    invalid["provider"] = "google";
    EXPECT_FALSE(call("webSearch.save", invalid)["ok"]);
    auto api_keys = call("apiKey.list")["result"];
    ASSERT_EQ(api_keys.size(), 1u);
    EXPECT_EQ(api_keys[0]["used_by"], nlohmann::json::array({"Search API"}));
    const auto provider = call("provider.get", {{"provider_id", "test"}});
    ASSERT_TRUE(provider["ok"]);
    EXPECT_EQ(std::ranges::find(provider["result"]["used_by"], "Search API"),
        provider["result"]["used_by"].end());

    auto turned_off = enabled;
    turned_off["tool_enabled"] = false;
    ASSERT_TRUE(call("webSearch.save", turned_off)["ok"]);
    EXPECT_EQ(call("webSearch.get")["result"], turned_off);
    api_keys = call("apiKey.list")["result"];
    EXPECT_EQ(api_keys[0]["used_by"], nlohmann::json::array());
    auto missing_tool_setting = enabled;
    missing_tool_setting.erase("tool_enabled");
    EXPECT_FALSE(call("webSearch.save", missing_tool_setting)["ok"]);
    invalid = enabled;
    invalid["api_key"] = "missing";
    EXPECT_FALSE(call("webSearch.save", invalid)["ok"]);
}

TEST_F(BridgeRouterTest, BootstrapIncludesVersionAndCapabilities) {
    const auto expected = application_->capabilities();
    router_->handle_request(
        connection_,
        request_json(connection_, next_id_++, 0, "app.bootstrap").dump());
    auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    const auto reply = reply_with_id(*batch, next_id_ - 1);
    ASSERT_TRUE(reply["ok"]);
    const auto& result = reply["result"];
    EXPECT_EQ(result["application_version"].get<std::string>(), kApplicationVersion);
    ASSERT_TRUE(result["capabilities"].is_object());
    EXPECT_EQ(result["capabilities"]["can_modify"], expected.can_modify);
    EXPECT_EQ(
        result["capabilities"]["can_transfer_r2"],
        expected.can_transfer_r2);
    ack_delivery(*router_, connection_, *batch);
}

TEST_F(BridgeRouterTest, ListsRenamesExportsAndEditsWorkspaceOverTheBridge) {
    bootstrap_epoch();
    auto reply = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Catalog"}});
    ASSERT_TRUE(reply["ok"]);
    const std::string session_id = reply["result"]["id"];

    reply = call("session.list", {{"forum_id", "lobby"}});
    ASSERT_TRUE(reply["ok"]);
    ASSERT_TRUE(reply["result"].is_array());
    EXPECT_EQ(reply["result"][0]["id"], session_id);
    EXPECT_EQ(reply["result"][0]["label"], "Catalog");
    EXPECT_FALSE(reply["result"][0]["live"]);

    reply = call(
        "session.rename",
        {{"forum_id", "lobby"}, {"session_id", session_id}, {"label", "Renamed"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["label"], "Renamed");

    reply = call(
        "session.export",
        {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_NE(
        reply["result"]["markdown"].get<std::string>().find("Renamed"),
        std::string::npos);

    reply = call(
        "session.open", {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);
    reply = call(
        "session.setDefaultCharacter",
        {{"forum_id", "lobby"},
         {"session_id", session_id},
         {"character_id", "guide"}});
    ASSERT_TRUE(reply["ok"]);
    reply = call(
        "session.uncover",
        {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);

    reply = call(
        "character.create",
        {{"display_name", "Mentor"}, {"description", "A guide"}});
    ASSERT_TRUE(reply["ok"]);
    const std::string character_id = reply["result"]["id"];
    EXPECT_EQ(reply["result"]["display_name"], "Mentor");

    reply = call("character.get", {{"character_id", character_id}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["id"], character_id);

    reply = call("persona.create", {{"display_name", "Narrator"}});
    ASSERT_TRUE(reply["ok"]);
    const std::string persona_id = reply["result"]["id"];

    reply = call(
        "forum.create",
        {{"display_name", "Workshop"}, {"persona_id", persona_id}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["display_name"], "Workshop");

    reply = call(
        "session.delete",
        {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);
}

TEST_F(BridgeRouterTest, ConditionalSessionDeletePreservesUsedAndNamedSessions) {
    bootstrap_epoch();
    const auto created = call("session.create", {{"forum_id", "lobby"}, {"label", ""}});
    ASSERT_TRUE(created["ok"]);
    const std::string id = created["result"]["id"];
    const auto discarded = call("session.delete",
        {{"forum_id", "lobby"}, {"session_id", id}, {"only_if_unused", true}});
    ASSERT_TRUE(discarded["ok"]);
    const auto opened = call("session.open", {{"forum_id", "lobby"}, {"session_id", id}});
    EXPECT_FALSE(opened["ok"]);
    EXPECT_EQ(opened["error"]["code"], "not_found");

    const auto named = call("session.create", {{"forum_id", "lobby"}, {"label", "Keep"}});
    ASSERT_TRUE(named["ok"]);
    const std::string named_id = named["result"]["id"];
    EXPECT_TRUE(call("session.delete", {{"forum_id", "lobby"}, {"session_id", named_id},
        {"only_if_unused", true}})["ok"]);
    EXPECT_TRUE(call("session.open", {{"forum_id", "lobby"}, {"session_id", named_id}})["ok"]);
    EXPECT_FALSE(call("session.delete", {{"forum_id", "lobby"}, {"session_id", named_id},
        {"only_if_unused", "yes"}})["ok"]);
}

TEST_F(BridgeRouterTest, RejectsMalformedUnknownStaleDuplicateAndUnavailableMethods) {
    bootstrap_epoch();
    const auto before = application_->live_session_count();

    router_->handle_request(connection_, "{\"not\":\"a request\"}");
    auto batch = wait_delivery(*router_, connection_, 200ms);
    EXPECT_FALSE(batch) << "Malformed requests without an id send no reply";

    auto reply = call("settings.save", nlohmann::json::object());
    ASSERT_FALSE(reply.empty());
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    EXPECT_EQ(reply["error"]["message"], "That method is not available.");

    reply = call(
        "session.submit",
        {{"forum_id", "lobby"},
         {"session_id", "missing"},
         {"input", {{"text", "Hello"}}}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "session_not_live");

    const auto id = next_id_++;
    const auto body = request_json(
        connection_, id, epoch_, "session.snapshot",
        {{"forum_id", "lobby"}, {"session_id", "missing"}}).dump();
    router_->handle_request(connection_, body);
    router_->handle_request(connection_, body);
    auto duplicate = router_->take_delivery(connection_);
    ASSERT_TRUE(duplicate);
    auto dup_reply = reply_with_id(*duplicate, id);
    EXPECT_FALSE(dup_reply["ok"]);
    EXPECT_EQ(dup_reply["error"]["code"], "invalid_argument");
    EXPECT_EQ(dup_reply["error"]["message"], "Duplicate outstanding request id.");
    ack_delivery(*router_, connection_, *duplicate);
    batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    auto admitted = reply_with_id(*batch, id);
    EXPECT_FALSE(admitted["ok"]);
    EXPECT_EQ(admitted["error"]["code"], "session_not_live");
    ack_delivery(*router_, connection_, *batch);

    // Wrong epoch: rebuild a request with a stale epoch.
    const auto stale_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(
            connection_,
            stale_id,
            epoch_ + 99,
            "session.create",
            {{"forum_id", "lobby"}, {"label", "Stale"}}).dump());
    batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    reply = reply_with_id(*batch, stale_id);
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "vault_changed");
    ack_delivery(*router_, connection_, *batch);

    reply = call("vault.create", {{"copy_from", nullptr}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");

    reply = call("vault.delete", {{"vault_name", 1}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");

    EXPECT_EQ(
        application_->live_session_count(), before);
}

TEST_F(BridgeRouterTest, TimesOutAdmittedWorkWithoutExecutingALateMutation) {
    router_ = std::make_unique<BridgeRouter>(
        *application_,
        BridgeRouter::Options{
            .platform = "test",
            .command_deadline = 0ms,
        });
    connection_ = router_->open_connection();
    bootstrap_epoch();
    const auto timeout_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(
            connection_,
            timeout_id,
            epoch_,
            "session.submit",
            {{"forum_id", "lobby"},
             {"session_id", "welcome"},
             {"input", {{"text", "Should not run"}}}}).dump());
    router_->expire_timeouts();
    auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    auto reply = reply_with_id(*batch, timeout_id);
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "command_timeout");
    ack_delivery(*router_, connection_, *batch);
    router_->run_tasks();
    EXPECT_FALSE(wait_delivery(*router_, connection_, 50ms));
}

TEST(CommandReply, LateCompletionAfterAbandonDoesNotReplay) {
    CommandReply reply;
    bool called = false;
    reply.set_ready_callback([&] { called = true; });
    reply.abandon();
    EXPECT_FALSE(reply.complete(CommandResult{.clear_input = true}));
    EXPECT_FALSE(called);
    EXPECT_FALSE(reply.peek());
}

TEST_F(BridgeRouterTest, SubscribeSnapshotAndAppendUseOneSequence) {
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Events"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(
        call(
            "session.open",
            {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);

    const auto sub_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(
            connection_,
            sub_id,
            epoch_,
            "session.subscribe",
            {{"forum_id", "lobby"},
             {"session_id", session_id},
             {"subscription_id", "sub-1"}}).dump());
    bool saw_reply = false;
    bool saw_snapshot = false;
    std::uint64_t snapshot_seq = 99;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline
        && (!saw_reply || !saw_snapshot)) {
        auto batch = wait_delivery(*router_, connection_);
        ASSERT_TRUE(batch);
        for (const auto& message : batch->at("messages")) {
            if (message.contains("id") && message["id"] == sub_id) {
                EXPECT_TRUE(message["ok"].get<bool>());
                saw_reply = true;
            }
            if (message.contains("event")
                && message["event"] == "session.snapshot") {
                EXPECT_EQ(message["subscription_id"], "sub-1");
                snapshot_seq = message["seq"];
                saw_snapshot = true;
            }
        }
        ack_delivery(*router_, connection_, *batch);
    }
    EXPECT_TRUE(saw_reply);
    EXPECT_TRUE(saw_snapshot);
    EXPECT_EQ(snapshot_seq, 0U);

    auto submitted = call(
        "session.submit",
        {{"forum_id", "lobby"},
         {"session_id", session_id},
         {"input", {{"text", "Hi"}}}});
    ASSERT_TRUE(submitted.contains("ok"));
    ASSERT_TRUE(submitted["ok"].get<bool>());
    auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    bool saw_update = false;
    for (const auto& message : batch->at("messages")) {
        if (!message.contains("event")) continue;
        EXPECT_GE(message["seq"].get<std::uint64_t>(), 1U);
        saw_update = true;
    }
    EXPECT_TRUE(saw_update);
    ack_delivery(*router_, connection_, *batch);

    const auto old_unsub = next_id_++;
    router_->handle_request(
        connection_,
        request_json(
            connection_,
            old_unsub,
            epoch_,
            "session.unsubscribe",
            {{"forum_id", "lobby"},
             {"session_id", session_id},
             {"subscription_id", "sub-stale"}}).dump());
    batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    auto reply = reply_with_id(*batch, old_unsub);
    ASSERT_TRUE(reply.contains("ok"));
    EXPECT_TRUE(reply["ok"].get<bool>());
    ack_delivery(*router_, connection_, *batch);

    auto replaced = call(
        "session.subscribe",
        {{"forum_id", "lobby"},
         {"session_id", session_id},
         {"subscription_id", "sub-2"}});
    ASSERT_TRUE(replaced.contains("ok"));
    EXPECT_TRUE(replaced["ok"].get<bool>());
}

TEST_F(BridgeRouterTest, ImmediateUnsubscribeCancelsPendingSubscribe) {
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Events"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(
        call(
            "session.open",
            {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);

    const auto subscribe_id = next_id_++;
    const auto unsubscribe_id = next_id_++;
    const nlohmann::json identity{
        {"forum_id", "lobby"},
        {"session_id", session_id},
        {"subscription_id", "sub-immediate"},
    };
    router_->handle_request(
        connection_,
        request_json(
            connection_, subscribe_id, epoch_, "session.subscribe", identity).dump());
    router_->handle_request(
        connection_,
        request_json(
            connection_, unsubscribe_id, epoch_, "session.unsubscribe", identity).dump());

    nlohmann::json subscribe_reply;
    nlohmann::json unsubscribe_reply;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((subscribe_reply.empty() || unsubscribe_reply.empty())
           && std::chrono::steady_clock::now() < deadline) {
        auto batch = wait_delivery(*router_, connection_, 200ms);
        ASSERT_TRUE(batch);
        if (subscribe_reply.empty()) {
            subscribe_reply = reply_with_id(*batch, subscribe_id);
        }
        if (unsubscribe_reply.empty()) {
            unsubscribe_reply = reply_with_id(*batch, unsubscribe_id);
        }
        ack_delivery(*router_, connection_, *batch);
    }
    ASSERT_FALSE(subscribe_reply.empty());
    EXPECT_FALSE(subscribe_reply["ok"]);
    EXPECT_EQ(subscribe_reply["error"]["code"], "operation_cancelled");
    ASSERT_FALSE(unsubscribe_reply.empty());
    EXPECT_TRUE(unsubscribe_reply["ok"]);
    EXPECT_FALSE(wait_delivery(*router_, connection_, 100ms));
}

TEST_F(BridgeRouterTest, UnsubscribeCleansSubscribeAlreadySentToOwner) {
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Events"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(
        call(
            "session.open",
            {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);

    const auto subscribe_id = next_id_++;
    const auto unsubscribe_id = next_id_++;
    const nlohmann::json identity{
        {"forum_id", "lobby"},
        {"session_id", session_id},
        {"subscription_id", "sub-owner-race"},
    };
    router_->handle_request(
        connection_,
        request_json(
            connection_, subscribe_id, epoch_, "session.subscribe", identity).dump());
    router_->run_tasks();
    router_->handle_request(
        connection_,
        request_json(
            connection_, unsubscribe_id, epoch_, "session.unsubscribe", identity).dump());

    nlohmann::json subscribe_reply;
    nlohmann::json unsubscribe_reply;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while ((subscribe_reply.empty() || unsubscribe_reply.empty())
           && std::chrono::steady_clock::now() < deadline) {
        auto batch = wait_delivery(*router_, connection_, 200ms);
        ASSERT_TRUE(batch);
        if (subscribe_reply.empty()) {
            subscribe_reply = reply_with_id(*batch, subscribe_id);
        }
        if (unsubscribe_reply.empty()) {
            unsubscribe_reply = reply_with_id(*batch, unsubscribe_id);
        }
        ack_delivery(*router_, connection_, *batch);
    }
    ASSERT_FALSE(subscribe_reply.empty());
    EXPECT_TRUE(subscribe_reply["ok"]);
    ASSERT_FALSE(unsubscribe_reply.empty());
    EXPECT_TRUE(unsubscribe_reply["ok"]);

    auto session = application_->subscription_handle("lobby", session_id);
    ASSERT_TRUE(session);
    const auto detached = std::chrono::steady_clock::now() + 2s;
    while (session->output()->attached()
           && std::chrono::steady_clock::now() < detached) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_FALSE(session->output()->attached());
}

TEST_F(BridgeRouterTest, ClosingConnectionReleasesAPendingOrCompletedSubscription) {
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Closing"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(call("session.open",
        {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);
    const auto session = application_->subscription_handle("lobby", session_id);
    ASSERT_TRUE(session);

    router_->handle_request(connection_, request_json(
        connection_, next_id_++, epoch_, "session.subscribe",
        {{"forum_id", "lobby"}, {"session_id", session_id},
         {"subscription_id", "sub-closing"}}).dump());
    router_->run_tasks();
    // Completion may already have installed ActiveSubscription or may still
    // be queued on the owner; both paths must enqueue matching cleanup.
    router_->close_connection(connection_);

    // An owner command after cleanup makes the detached assertion deterministic.
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(
        session->snapshot(2s)));
    EXPECT_FALSE(session->output()->attached());
    EXPECT_FALSE(router_->take_delivery(connection_));
}

TEST_F(BridgeRouterTest, RetainsReplyUntilAckAndIgnoresLateAck) {
    bootstrap_epoch();
    const auto id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(connection_, id, epoch_, "bridge.info").dump());
    auto first = wait_delivery(*router_, connection_);
    ASSERT_TRUE(first);
    EXPECT_FALSE(router_->take_delivery(connection_));
    router_->handle_ack(
        connection_,
        nlohmann::json{
            {"connection_id", connection_},
            {"delivery_id", first->at("delivery_id").get<std::uint64_t>() + 9},
        }.dump());
    EXPECT_FALSE(router_->take_delivery(connection_));
    ack_delivery(*router_, connection_, *first);
    EXPECT_FALSE(router_->take_delivery(connection_));
}

TEST_F(BridgeRouterTest, EmptyDrainsDoNotConsumeDeliveryIds) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        EXPECT_FALSE(router_->take_delivery(connection_));
    }
    router_->handle_request(
        connection_, request_json(connection_, 1, 0, "bridge.info").dump());
    auto delivery = wait_delivery(*router_, connection_);
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->at("delivery_id"), 1U);
    ack_delivery(*router_, connection_, *delivery);
}

TEST_F(BridgeRouterTest, RequestIdsMustIncreaseWithinAConnection) {
    router_->handle_request(
        connection_, request_json(connection_, 2, 0, "bridge.info").dump());
    auto delivery = wait_delivery(*router_, connection_);
    ASSERT_TRUE(delivery);
    EXPECT_TRUE(reply_with_id(*delivery, 2)["ok"]);
    ack_delivery(*router_, connection_, *delivery);

    router_->handle_request(
        connection_, request_json(connection_, 1, 0, "bridge.info").dump());
    delivery = wait_delivery(*router_, connection_);
    ASSERT_TRUE(delivery);
    const auto rejected = reply_with_id(*delivery, 1);
    ASSERT_FALSE(rejected.empty());
    EXPECT_FALSE(rejected["ok"]);
    EXPECT_EQ(rejected["error"]["message"], "Request ids must increase.");
    ack_delivery(*router_, connection_, *delivery);

    router_->handle_request(
        connection_, request_json(connection_, 3, 0, "bridge.info").dump());
    delivery = wait_delivery(*router_, connection_);
    ASSERT_TRUE(delivery);
    EXPECT_TRUE(reply_with_id(*delivery, 3)["ok"]);
    ack_delivery(*router_, connection_, *delivery);
}

TEST_F(BridgeRouterTest, ZeroAndStaleRequestsCannotMutateAfterVaultSwitch) {
    const auto old_epoch = bootstrap_epoch();
    const auto created = application_->create_vault(VaultCreate{
        .display_name = "Copied",
        .copy_from = "Test",
        .password = {},
    }, old_epoch);
    const auto switched = application_->switch_vault(created.name, {}, old_epoch);
    auto notification = wait_delivery(*router_, connection_);
    ASSERT_TRUE(notification);
    ack_delivery(*router_, connection_, *notification);
    const auto before = application_->list_sessions(
        "lobby", switched.context_epoch).size();

    for (const auto invalid_epoch : {std::uint64_t{0}, old_epoch}) {
        SCOPED_TRACE(invalid_epoch);
        epoch_ = invalid_epoch;
        const auto reply = call(
            "session.create", {{"forum_id", "lobby"}, {"label", "Rejected"}});
        EXPECT_FALSE(reply.at("ok"));
        EXPECT_EQ(reply.at("error").at("code"),
            invalid_epoch == 0 ? "invalid_argument" : "vault_changed");
    }
    EXPECT_EQ(application_->list_sessions(
        "lobby", switched.context_epoch).size(), before);

    epoch_ = switched.context_epoch;
    EXPECT_TRUE(call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Accepted"}}).at("ok"));
    EXPECT_EQ(application_->list_sessions(
        "lobby", switched.context_epoch).size(), before + 1);
}

TEST_F(BridgeRouterTest, ContextNotificationsCoalesceBehindInFlightDelivery) {
    bootstrap_epoch();
    const auto created = application_->create_vault(VaultCreate{
        .display_name = "Copied",
        .copy_from = "Test",
        .password = {},
    }, epoch_);

    const auto request_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(connection_, request_id, epoch_, "bridge.info").dump());
    auto held = wait_delivery(*router_, connection_);
    ASSERT_TRUE(held);

    const auto switched = application_->switch_vault(
        created.name, {}, application_->context_epoch());
    const auto restored = application_->switch_vault(
        "Test", {}, switched.context_epoch);
    EXPECT_FALSE(router_->take_delivery(connection_));

    ack_delivery(*router_, connection_, *held);
    auto notification = wait_delivery(*router_, connection_);
    ASSERT_TRUE(notification);
    ASSERT_EQ(notification->at("messages").size(), 1U);
    const auto& message = notification->at("messages").front();
    EXPECT_EQ(message["event"], "app.contextChanged");
    EXPECT_EQ(message["context_epoch"], restored.context_epoch);
    EXPECT_FALSE(message.contains("causing_request_id"));
    ack_delivery(*router_, connection_, *notification);
}

TEST_F(BridgeRouterTest, ContextNotificationNamesOnlyTheOriginatingRequest) {
    bootstrap_epoch();
    const auto created = application_->create_vault({.display_name = "Other"}, epoch_);
    const std::string observer = router_->open_connection();
    const auto request_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(connection_, request_id, epoch_, "vault.switch",
            {{"vault_name", created.name}, {"password", nullptr}}).dump());

    const auto initiating_batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(initiating_batch);
    const auto& initiating_event = initiating_batch->at("messages").front();
    EXPECT_EQ(initiating_event["event"], "app.contextChanged");
    EXPECT_EQ(initiating_event["causing_request_id"], request_id);
    ack_delivery(*router_, connection_, *initiating_batch);

    const auto observer_batch = wait_delivery(*router_, observer);
    ASSERT_TRUE(observer_batch);
    const auto& observer_event = observer_batch->at("messages").front();
    EXPECT_EQ(observer_event["event"], "app.contextChanged");
    EXPECT_FALSE(observer_event.contains("causing_request_id"));
    ack_delivery(*router_, observer, *observer_batch);
}

TEST_F(BridgeRouterTest, ForumDeletionPreservesItsReplyAcrossMaintenanceAndReportsOtherUsers) {
    bootstrap_epoch();
    for (const bool blocked : {true, false}) {
        SCOPED_TRACE(blocked);
        auto secondary = blocked ? WorkspaceConfigStore::open(database_) : nullptr;
        epoch_ = application_->context_epoch();
        const auto id = next_id_++;
        router_->handle_request(connection_, request_json(connection_, id, epoch_,
            "forum.delete", {{"forum_id", "lobby"}}).dump());
        nlohmann::json reply;
        bool context_seen = false;
        for (int i = 0; i < 4 && reply.empty(); ++i) {
            const auto batch = wait_delivery(*router_, connection_);
            ASSERT_TRUE(batch);
            for (const auto& message : batch->at("messages")) {
                if (message.value("event", "") == "app.contextChanged") {
                    EXPECT_EQ(message["causing_request_id"], id);
                    context_seen = true;
                }
            }
            reply = reply_with_id(*batch, id);
            ack_delivery(*router_, connection_, *batch);
        }
        ASSERT_FALSE(reply.empty());
        EXPECT_TRUE(context_seen);
        EXPECT_GT(reply["context_epoch"].get<std::uint64_t>(), epoch_);
        EXPECT_EQ(reply["ok"], !blocked);
        if (blocked) {
            EXPECT_EQ(reply["error"]["code"], "invalid_argument");
            EXPECT_EQ(reply["error"]["message"],
                "Close other CHA processes using this vault, then try again.");
            EXPECT_NE(application_->store().snapshot()->find_forum("lobby"), nullptr);
        } else {
            EXPECT_EQ(application_->store().snapshot()->find_forum("lobby"), nullptr);
        }
        EXPECT_EQ(application_->state(), app::ApplicationState::running);
    }
}

TEST_F(BridgeRouterTest, CompletedReplyHoldsAdmissionUntilDeliveryAck) {
    router_ = std::make_unique<BridgeRouter>(
        *application_,
        BridgeRouter::Options{.platform = "test", .ordinary_limit = 1});
    connection_ = router_->open_connection();

    router_->handle_request(
        connection_, request_json(connection_, 1, 0, "bridge.info").dump());
    router_->run_tasks();
    auto first = router_->take_delivery(connection_);
    ASSERT_TRUE(first);
    EXPECT_TRUE(reply_with_id(*first, 1)["ok"]);

    router_->handle_request(
        connection_, request_json(connection_, 2, 0, "bridge.info").dump());
    EXPECT_FALSE(router_->take_delivery(connection_));
    ack_delivery(*router_, connection_, *first);

    auto overflow = wait_delivery(*router_, connection_);
    ASSERT_TRUE(overflow);
    const auto reply = reply_with_id(*overflow, 2);
    ASSERT_FALSE(reply.empty());
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["message"], "Too many in-flight requests.");
    EXPECT_EQ(reply["error"]["code"], "command_queue_full");
    ack_delivery(*router_, connection_, *overflow);
}

TEST_F(BridgeRouterTest, SaturatingOrdinaryWorkInvalidatesTheConnection) {
    router_ = std::make_unique<BridgeRouter>(
        *application_,
        BridgeRouter::Options{.platform = "test", .ordinary_limit = 1});
    connection_ = router_->open_connection();
    router_->handle_request(
        connection_,
        request_json(connection_, 1, 0, "app.bootstrap").dump());
    router_->handle_request(
        connection_,
        request_json(connection_, 2, 0, "bridge.info").dump());
    auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    bool saw_limit = false;
    bool saw_cancelled = false;
    bool saw_invalidation = false;
    for (const auto& message : batch->at("messages")) {
        if (message.contains("error")
            && message["error"]["message"] == "Too many in-flight requests.") {
            EXPECT_EQ(message["id"], 2);
            EXPECT_EQ(message["error"]["code"], "command_queue_full");
            saw_limit = true;
        }
        if (message.contains("id") && message["id"] == 1
            && message.contains("error")
            && message["error"]["code"] == "operation_cancelled") {
            saw_cancelled = true;
        }
        if (message.value("event", "") == "app.connectionInvalidated"
            && message.value("reason", "") == "request_limit_exceeded") {
            saw_invalidation = true;
        }
    }
    EXPECT_TRUE(saw_limit);
    EXPECT_TRUE(saw_cancelled);
    EXPECT_TRUE(saw_invalidation);
    router_->handle_request(
        connection_,
        request_json(connection_, 3, 0, "bridge.info").dump());
    EXPECT_FALSE(wait_delivery(*router_, connection_, 50ms));
}

TEST_F(BridgeRouterTest, StalledAckKeepsOneOutstandingDelivery) {
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Stall"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(
        call(
            "session.open",
            {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);
    ASSERT_TRUE(
        call(
            "session.subscribe",
            {{"forum_id", "lobby"},
             {"session_id", session_id},
             {"subscription_id", "sub-stall"}})["ok"]);
    auto snapshot = wait_delivery(*router_, connection_, 2s);
    if (snapshot) ack_delivery(*router_, connection_, *snapshot);

    ASSERT_TRUE(
        call(
            "session.submit",
            {{"forum_id", "lobby"},
             {"session_id", session_id},
             {"input", {{"text", "Burst"}}}})["ok"]);
    auto first = wait_delivery(*router_, connection_);
    ASSERT_TRUE(first);
    EXPECT_FALSE(router_->take_delivery(connection_));
    std::this_thread::sleep_for(20ms);
    router_->run_tasks();
    router_->pump_output();
    EXPECT_FALSE(router_->take_delivery(connection_));
    ack_delivery(*router_, connection_, *first);
    auto stopped = call(
        "session.stop", {{"forum_id", "lobby"}, {"session_id", session_id}});
    EXPECT_TRUE(stopped["ok"]);
}

TEST_F(BridgeRouterTest, NewConnectionReusesIdsWithoutResolvingOldReplies) {
    bootstrap_epoch();
    const auto old_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(connection_, old_id, epoch_, "bridge.info").dump());
    auto old_batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(old_batch);
    const auto old_delivery = old_batch->at("delivery_id").get<std::uint64_t>();
    router_->close_connection(connection_);
    connection_ = router_->open_connection();
    next_id_ = 1;
    bootstrap_epoch();
    router_->handle_ack(
        connection_,
        nlohmann::json{
            {"connection_id", connection_},
            {"delivery_id", old_delivery},
        }.dump());
    auto reply = call("bridge.info");
    ASSERT_TRUE(reply["ok"]);
}

TEST_F(BridgeRouterTest, ShutdownDoesNotWaitForRendererAck) {
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Quit"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(
        call(
            "session.open",
            {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);
    ASSERT_TRUE(
        call(
            "session.submit",
            {{"forum_id", "lobby"},
             {"session_id", session_id},
             {"input", {{"text", "Still running"}}}})["ok"]);
    auto pending = wait_delivery(*router_, connection_, 500ms);
    router_->shutdown();
    application_->request_shutdown();
    EXPECT_TRUE(application_->join_shutdown(2s));
    if (pending) {
        router_->handle_ack(
            connection_,
            nlohmann::json{
                {"connection_id", connection_},
                {"delivery_id", pending->at("delivery_id")},
            }.dump());
    }
}

TEST_F(BridgeRouterTest, MaintenanceRequestsIgnoreTheOrdinaryCommandDeadline) {
    router_ = std::make_unique<BridgeRouter>(
        *application_,
        BridgeRouter::Options{
            .platform = "test",
            .command_deadline = 0ms,
        });
    connection_ = router_->open_connection();
    bootstrap_epoch();

    const std::vector<std::pair<std::string, nlohmann::json>> requests{
        {"vault.merge", nlohmann::json::object()},
        {"vault.upload", {{"etag", "expected-etag"}}},
        {"vault.download", nlohmann::json::object()},
        {"vault.import", nlohmann::json::object()},
        {"vault.export", nlohmann::json::object()},
        {"vault.r2.download", {{"name", "remote"}}},
    };
    for (const auto& [method, params] : requests) {
        const auto id = next_id_++;
        router_->handle_request(
            connection_,
            request_json(connection_, id, epoch_, method, params).dump());
        router_->expire_timeouts();
        const auto batch = wait_delivery(*router_, connection_);
        ASSERT_TRUE(batch) << method;
        const auto reply = reply_with_id(*batch, id);
        ASSERT_FALSE(reply.empty()) << method;
        EXPECT_NE(reply.value("error", nlohmann::json::object()).value(
            "code", ""), "command_timeout") << method;
        for (const auto& message : batch->at("messages")) {
            if (message.value("event", "") == "app.contextChanged") {
                epoch_ = message.at("context_epoch").get<std::uint64_t>();
            }
        }
        ack_delivery(*router_, connection_, *batch);
    }
}

TEST_F(BridgeRouterTest, MaintenanceMethodsRejectParameters) {
    bootstrap_epoch();
    for (const std::string_view method : {
             "vault.download", "vault.import", "vault.export"}) {
        SCOPED_TRACE(method);
        const auto reply = call(method, {{"unexpected", true}});
        ASSERT_FALSE(reply["ok"]);
        EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    }
    const auto upload = call(
        "vault.upload", {{"etag", "expected"}, {"unexpected", true}});
    ASSERT_FALSE(upload["ok"]);
    EXPECT_EQ(upload["error"]["code"], "invalid_argument");
}

TEST_F(BridgeRouterTest, MaintenanceFailuresKeepTheirReasonAndRecoveredEpoch) {
    bootstrap_epoch();
    const std::vector<std::tuple<std::string, nlohmann::json, std::string>> requests{
        {"vault.upload", {{"etag", "expected-etag"}}, "The active vault has no R2 key"},
        {"vault.download", nlohmann::json::object(), "The active vault has no R2 key"},
        {"vault.import", nlohmann::json::object(), "Application config requires 'modify' for Import"},
        {"vault.export", nlohmann::json::object(), "Application config requires 'modify' for Export"},
    };
    for (const auto& [method, params, message] : requests) {
        SCOPED_TRACE(method);
        const auto old_epoch = epoch_;
        const auto reply = call(method, params);
        ASSERT_FALSE(reply["ok"]);
        EXPECT_EQ(reply["error"]["code"], "invalid_argument");
        EXPECT_EQ(reply["error"]["message"], message);
        epoch_ = reply["context_epoch"].get<std::uint64_t>();
        EXPECT_GT(epoch_, old_epoch);
    }
}

TEST_F(BridgeRouterTest, MaintenanceHttpStatusKeepsSafeReason) {
    bootstrap_epoch();
    MockHttpServer server({
        "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n",
    });
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket/",
        .access_key_id = "access",
        .secret_key = std::string("secret"),
    }, epoch_);
    server.start();
    const auto reply = call("vault.upload", {{"etag", nullptr}});
    server.join();
    ASSERT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    EXPECT_EQ(reply["error"]["message"],
        "R2 upload failed with HTTP status 403");
    EXPECT_GT(reply["context_epoch"].get<std::uint64_t>(), epoch_);
}

TEST_F(BridgeRouterTest, UploadCheckHttpStatusKeepsSafeReason) {
    bootstrap_epoch();
    MockHttpServer server({
        etag_response("uploader-etag"),
        etag_response("uploaded-etag"),
        etag_response("vault-etag"),
        "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n",
    });
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket/",
        .access_key_id = "access",
        .secret_key = std::string("secret"),
    }, epoch_);
    server.start();
    const auto uploaded = call("vault.upload", {{"etag", nullptr}});
    ASSERT_TRUE(uploaded["ok"]);
    epoch_ = uploaded["result"]["context_epoch"].get<std::uint64_t>();
    const auto reply = call("vault.upload.check");
    server.join();

    ASSERT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    EXPECT_EQ(reply["error"]["message"],
        "R2 metadata request failed with HTTP status 403");
    EXPECT_EQ(reply["context_epoch"], epoch_);
    EXPECT_EQ(application_->context_epoch(), epoch_);
    ASSERT_EQ(server.requests().size(), 4U);
    EXPECT_TRUE(server.requests().back().starts_with("HEAD "));
}

TEST_F(BridgeRouterTest, MaintenanceUploadConflictExplainsHowToRetry) {
    bootstrap_epoch();
    MockHttpServer server({
        etag_response("uploader-etag"),
        "HTTP/1.1 412 Precondition Failed\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n",
    });
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket/",
        .access_key_id = "access",
        .secret_key = std::string("secret"),
    }, epoch_);
    server.start();
    const auto reply = call("vault.upload", {{"etag", "previous-etag"}});
    server.join();

    ASSERT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    EXPECT_EQ(reply["error"]["message"],
        "The R2 vault changed since the check. Click Upload again to review it.");
    EXPECT_GT(reply["context_epoch"].get<std::uint64_t>(), epoch_);
    ASSERT_EQ(server.requests().size(), 2U);
    EXPECT_NE(server.requests()[1].find("If-Match: \"previous-etag\""),
        std::string::npos);
}

TEST_F(BridgeRouterTest, ExportReplacesExistingDestinationContents) {
    router_->shutdown();
    application_->request_shutdown();
    ASSERT_TRUE(application_->join_shutdown(2s));
    router_.reset();
    application_.reset();

    auto command = make_command(workspace_, database_);
    const auto modify = workspace_.root() / "modify";
    command.vault.modify = modify;
    command.vaults.front().modify = modify;
    application_ = app::Application::open(std::move(command));
    router_ = std::make_unique<BridgeRouter>(*application_);
    connection_ = router_->open_connection();
    bootstrap_epoch();

    std::filesystem::create_directories(modify);
    std::ofstream(modify / "private-note.txt") << "not a CHA workspace";
    const auto reply = call("vault.export");
    ASSERT_TRUE(reply["ok"]);
    EXPECT_GT(reply["result"]["file_count"].get<std::size_t>(), 0U);
    EXPECT_FALSE(std::filesystem::exists(modify / "private-note.txt"));
    EXPECT_TRUE(std::filesystem::exists(modify / "system/providers/test/config.toml"));
    EXPECT_GT(reply["context_epoch"].get<std::uint64_t>(), epoch_);
}

TEST_F(BridgeRouterTest, ImportShowsSafeFileValidationReason) {
    router_->shutdown();
    application_->request_shutdown();
    ASSERT_TRUE(application_->join_shutdown(2s));
    router_.reset();
    application_.reset();

    auto command = make_command(workspace_, database_);
    const auto modify = workspace_.root() / "modify";
    command.vault.modify = modify;
    command.vaults.front().modify = modify;
    application_ = app::Application::open(std::move(command));
    router_ = std::make_unique<BridgeRouter>(*application_);
    connection_ = router_->open_connection();
    bootstrap_epoch();

    const auto exported = call("vault.export");
    ASSERT_TRUE(exported["ok"]);
    epoch_ = exported["result"]["context_epoch"].get<std::uint64_t>();
    std::ofstream(modify / "system/providers/test/config.toml")
        << "host = 42\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n";

    const auto failed = call("vault.import");
    ASSERT_FALSE(failed["ok"]);
    EXPECT_EQ(failed["error"]["code"], "invalid_argument");
    std::string message = failed["error"]["message"];
    std::replace(message.begin(), message.end(), '\\', '/');
    SCOPED_TRACE(message);
    EXPECT_NE(message.find("system/providers/test/config.toml"), std::string::npos);
    EXPECT_EQ(message.find("workspace/system"), std::string::npos);
    EXPECT_NE(message.find("host"), std::string::npos);
    EXPECT_EQ(message.find("/workspace"), std::string::npos);
    EXPECT_EQ(message.find(workspace_.root().string()), std::string::npos);
    EXPECT_GT(failed["context_epoch"].get<std::uint64_t>(), epoch_);

    epoch_ = failed["context_epoch"].get<std::uint64_t>();
    std::ofstream(modify / "system/providers/test/config.toml") << "not toml\n";
    const auto malformed = call("vault.import");
    ASSERT_FALSE(malformed["ok"]);
    EXPECT_EQ(malformed["error"]["code"], "invalid_argument");
    const std::string syntax = malformed["error"]["message"];
    EXPECT_NE(syntax.find("invalid provider 'test'"), std::string::npos);
    EXPECT_NE(syntax.find("parsing"), std::string::npos);
    EXPECT_EQ(syntax.find("/workspace"), std::string::npos);
    EXPECT_EQ(syntax.find(workspace_.root().string()), std::string::npos);

    epoch_ = malformed["context_epoch"].get<std::uint64_t>();
    std::filesystem::copy_file(
        workspace_.root() / "system/providers/test/config.toml",
        modify / "system/providers/test/config.toml",
        std::filesystem::copy_options::overwrite_existing);
    std::filesystem::remove(modify / "characters/guide/CHARACTER.md");
    const auto missing = call("vault.import");
    ASSERT_FALSE(missing["ok"]);
    EXPECT_EQ(missing["error"]["code"], "invalid_argument");
    const std::string required = missing["error"]["message"];
    EXPECT_NE(required.find("Character 'guide' requires character.toml and CHARACTER.md"),
        std::string::npos);
    EXPECT_EQ(required.find(workspace_.root().string()), std::string::npos);
}

TEST_F(BridgeRouterTest, MaintenanceResultsReturnCountsAndNewEpochs) {
    router_->shutdown();
    application_->request_shutdown();
    ASSERT_TRUE(application_->join_shutdown(2s));
    router_.reset();
    application_.reset();

    auto command = make_command(workspace_, database_);
    const auto modify = workspace_.root() / "modify";
    command.vault.modify = modify;
    command.vaults.front().modify = modify;
    application_ = app::Application::open(std::move(command));
    router_ = std::make_unique<BridgeRouter>(*application_);
    connection_ = router_->open_connection();
    bootstrap_epoch();

    auto check_result = [&](const nlohmann::json& reply, std::string_view field) {
        ASSERT_TRUE(reply["ok"]);
        ASSERT_TRUE(reply["result"].contains(field));
        EXPECT_TRUE(reply["result"][field].is_number_unsigned());
        const auto next_epoch = reply["result"]["context_epoch"].get<std::uint64_t>();
        EXPECT_EQ(reply["context_epoch"], next_epoch);
        EXPECT_GT(next_epoch, epoch_);
        epoch_ = next_epoch;
    };

    check_result(call("vault.export"), "file_count");
    check_result(call("vault.import"), "file_count");

    MockHttpServer upload_server({
        etag_response("uploader-etag"),
        etag_response("uploaded-etag"),
        etag_response("vault-etag"),
        etag_response("uploaded-etag"),
        etag_response("changed-etag"),
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    });
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(upload_server.port()) + "/bucket/",
        .access_key_id = "access",
        .secret_key = std::string("secret"),
    }, epoch_);
    const auto missing_check = call("vault.upload.check");
    ASSERT_TRUE(missing_check["ok"]);
    EXPECT_EQ(missing_check["result"]["etag"], nullptr);
    EXPECT_EQ(missing_check["result"]["status"], "missing");
    EXPECT_EQ(missing_check["result"]["context_epoch"], epoch_);
    upload_server.start();
    check_result(call("vault.upload", {{"etag", nullptr}}), "byte_count");
    const auto upload_check = call("vault.upload.check");
    ASSERT_TRUE(upload_check["ok"]);
    EXPECT_EQ(upload_check["result"]["etag"], "uploaded-etag");
    EXPECT_EQ(upload_check["result"]["status"], "match");
    EXPECT_EQ(upload_check["result"]["context_epoch"], epoch_);
    const auto changed_check = call("vault.upload.check");
    ASSERT_TRUE(changed_check["ok"]);
    EXPECT_EQ(changed_check["result"]["etag"], "changed-etag");
    EXPECT_EQ(changed_check["result"]["status"], "mismatch");
    const auto deleted_check = call("vault.upload.check");
    ASSERT_TRUE(deleted_check["ok"]);
    EXPECT_EQ(deleted_check["result"]["etag"], nullptr);
    EXPECT_EQ(deleted_check["result"]["status"], "mismatch");
    upload_server.join();

    test::TestWorkspace remote_workspace;
    remote_workspace.add_persona("remote", "Remote");
    const auto remote_database = test::import_test_database(remote_workspace.root());
    std::ifstream remote_input(remote_database, std::ios::binary);
    const std::string remote_bytes{
        std::istreambuf_iterator<char>(remote_input),
        std::istreambuf_iterator<char>()};
    const std::string remote_vault =
        "vault_name = \"Test\"\n"
        "data = \"/remote/" + database_.filename().string() + "\"\n";
    MockHttpServer download_server({
        http_response("application/toml", remote_vault),
        "HTTP/1.1 200 OK\r\nETag: \"remote-etag\"\r\nContent-Length: "
            + std::to_string(remote_bytes.size())
            + "\r\nConnection: close\r\n\r\n" + remote_bytes,
    });
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(download_server.port()) + "/bucket/",
        .access_key_id = "access",
        .secret_key = std::string("secret"),
    }, epoch_);
    download_server.start();
    check_result(call("vault.download"), "byte_count");
    download_server.join();
    EXPECT_EQ(application_->vault_snapshot().active.r2_etag, "remote-etag");
}

TEST_F(BridgeRouterTest, QueuedOldContextReplyIsInvalidatedBeforeDelivery) {
    bootstrap_epoch();
    ASSERT_TRUE(call("vault.create", {{"display_name", "Other"},
        {"copy_from", nullptr}, {"password", nullptr}})["ok"]);
    const auto id = next_id_++;
    router_->handle_request(connection_,
        request_json(connection_, id, epoch_, "provider.list").dump());
    router_->run_tasks(); // The result is ready, but the renderer has not taken it.
    const auto switched = application_->switch_vault("Other", {}, epoch_);
    const auto batch = router_->take_delivery(connection_);
    ASSERT_TRUE(batch);
    ASSERT_FALSE(batch->at("messages").empty());
    EXPECT_EQ(batch->at("messages").front()["event"], "app.contextChanged");
    EXPECT_EQ(batch->at("messages").front()["context_epoch"], switched.context_epoch);
    const auto reply = reply_with_id(*batch, id);
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "vault_changed");
    ack_delivery(*router_, connection_, *batch);
}

TEST_F(BridgeRouterTest, BusyVaultLeasePreservesTheCurrentContextAndSession) {
    bootstrap_epoch();
    const auto target = application_->create_vault({.display_name = "Busy"}, epoch_);
    const auto busy = SessionLease::acquire(target.data, "test lease");
    const auto session = call("session.create", {{"forum_id", "lobby"}, {"label", "Kept"}});
    const auto identity = nlohmann::json{{"forum_id", "lobby"},
        {"session_id", session["result"]["id"]}};
    ASSERT_TRUE(call("session.open", identity)["ok"]);
    EXPECT_FALSE(call("vault.switch", {{"vault_name", "Busy"}, {"password", nullptr}})["ok"]);
    EXPECT_FALSE(call("vault.merge", {{"source_vault", "Busy"}, {"password", nullptr}})["ok"]);
    EXPECT_EQ(application_->context_epoch(), epoch_);
    EXPECT_TRUE(call("session.snapshot", identity)["ok"]);
}

TEST_F(BridgeRouterTest, FailedMergeRecoveryKeepsShellAvailableAndRejectsDomainWork) {
    bootstrap_epoch();
    (void)application_->create_vault({.display_name = "Source"}, epoch_);
    force_next_forum_sync_failure();
    const auto failed = call("vault.merge", {{"source_vault", "Source"}, {"password", nullptr}});
    ASSERT_FALSE(failed["ok"]);
    EXPECT_EQ(failed["error"]["code"], "application_unavailable");
    EXPECT_EQ(failed["error"]["message"], "The application is unavailable.");
    EXPECT_TRUE(call("bridge.info")["ok"]);
    const auto boot = call("app.bootstrap");
    ASSERT_TRUE(boot["ok"]);
    EXPECT_EQ(boot["result"]["state"], "unavailable");
    const auto denied = call("session.create", {{"forum_id", "lobby"}, {"label", "Blocked"}});
    EXPECT_FALSE(denied["ok"]);
    EXPECT_EQ(denied["error"]["code"], "application_unavailable");
}

TEST_F(BridgeRouterTest, SnapshotCaptureAndSerializeStayBounded) {
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Measure"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(
        call(
            "session.open",
            {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);
    ASSERT_TRUE(
        call(
            "session.submit",
            {{"forum_id", "lobby"},
             {"session_id", session_id},
             {"input", {{"text", std::string(4000, 'x')}}}})["ok"]);
    const auto started = std::chrono::steady_clock::now();
    auto reply = call(
        "session.snapshot",
        {{"forum_id", "lobby"}, {"session_id", session_id}});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    ASSERT_TRUE(reply["ok"]);
    const auto serialized = reply["result"].dump();
    RecordProperty("snapshot_bytes", static_cast<int>(serialized.size()));
    RecordProperty(
        "snapshot_ms",
        static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count()));
    EXPECT_LT(elapsed, 5s);
}

TEST_F(BridgeRouterTest, StopRemainsAdmittedWhenOrdinaryWorkIsFull) {
    router_ = std::make_unique<BridgeRouter>(
        *application_,
        BridgeRouter::Options{.platform = "test", .ordinary_limit = 1});
    connection_ = router_->open_connection();
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Control"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(
        call(
            "session.open",
            {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);

    const auto bootstrap_id = next_id_++;
    const auto stop_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(connection_, bootstrap_id, epoch_, "app.bootstrap").dump());
    router_->handle_request(
        connection_,
        request_json(
            connection_,
            stop_id,
            epoch_,
            "session.stop",
            {{"forum_id", "lobby"}, {"session_id", session_id}}).dump());
    auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    auto reply = reply_with_id(*batch, stop_id);
    if (reply.empty()) {
        ack_delivery(*router_, connection_, *batch);
        batch = wait_delivery(*router_, connection_);
        ASSERT_TRUE(batch);
        reply = reply_with_id(*batch, stop_id);
    }
    ASSERT_FALSE(reply.empty());
    EXPECT_TRUE(reply.contains("ok"));
    ack_delivery(*router_, connection_, *batch);
}

TEST_F(BridgeRouterTest, ListsProvidersAndApiKeyMetadataWithoutSecrets) {
    bootstrap_epoch();
    auto reply = call("provider.list");
    ASSERT_TRUE(reply["ok"]);
    ASSERT_FALSE(reply["result"].empty());
    EXPECT_EQ(reply["result"].front().at("id"), "test");

    reply = call(
        "apiKey.create",
        {{"display_name", "Router"}, {"value", "private-router-secret"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply.dump().find("private-router-secret"), std::string::npos);
    EXPECT_TRUE(reply["result"].at("has_value").get<bool>());

    reply = call("openaiAuth.get");
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"].at("status"), "signed_out");

    reply = call("r2Storage.get");
    ASSERT_TRUE(reply["ok"]);
    if (!reply["result"].is_null()) {
        EXPECT_TRUE(reply["result"].contains("has_secret_key"));
        EXPECT_FALSE(reply["result"].contains("secret_key"));
    }
}

TEST_F(BridgeRouterTest, SlowProviderTestDoesNotStarveStop) {
    bootstrap_epoch();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Control"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    ASSERT_TRUE(
        call(
            "session.open",
            {{"forum_id", "lobby"}, {"session_id", session_id}})["ok"]);

    auto provider = call("provider.get", {{"provider_id", "test"}});
    ASSERT_TRUE(provider["ok"]);
    nlohmann::json body = provider["result"];
    body.erase("id");
    body.erase("used_by");
    body.erase("writable");
    MockHttpServer model_server(
        {http_response("text/event-stream", "data: {\"type\":\"response.completed\"}\n\n")},
        false,
        1500ms);
    model_server.start();
    body["host"] = "127.0.0.1";
    body["port"] = model_server.port();
    body["https"] = false;
    body["api"] = "responses";
    body["mode"] = "net";
    nlohmann::json test_params = body;
    test_params["provider_id"] = "test";

    const auto test_id = next_id_++;
    const auto stop_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(connection_, test_id, epoch_, "provider.test", test_params)
            .dump());
    router_->handle_request(
        connection_,
        request_json(
            connection_,
            stop_id,
            epoch_,
            "session.stop",
            {{"forum_id", "lobby"}, {"session_id", session_id}}).dump());
    auto batch = wait_delivery(*router_, connection_, 2s);
    ASSERT_TRUE(batch);
    auto stop = reply_with_id(*batch, stop_id);
    if (stop.empty()) {
        ack_delivery(*router_, connection_, *batch);
        batch = wait_delivery(*router_, connection_, 2s);
        ASSERT_TRUE(batch);
        stop = reply_with_id(*batch, stop_id);
    }
    ASSERT_FALSE(stop.empty());
    EXPECT_TRUE(stop.contains("ok"));
    ack_delivery(*router_, connection_, *batch);
}

TEST_F(BridgeRouterTest, HumanAudioRequestsReturnInvalidArgument) {
    bootstrap_epoch();
    const auto created = call("session.create", {{"forum_id", "lobby"}, {"label", "Audio"}});
    ASSERT_TRUE(created["ok"]);
    const auto session_id = created["result"]["id"];
    const nlohmann::json session{{"forum_id", "lobby"}, {"session_id", session_id}};
    ASSERT_TRUE(call("session.open", session)["ok"]);
    auto submission = session;
    submission["input"] = {{"text", "Hello"}};
    ASSERT_TRUE(call("session.submit", submission)["ok"]);
    ASSERT_TRUE(call("session.stop", session)["ok"]);
    const auto snapshot = call("session.snapshot", session);
    ASSERT_TRUE(snapshot["ok"]);
    const auto& entries = snapshot["result"]["transcript"];
    const auto human = std::find_if(entries.begin(), entries.end(), [](const auto& entry) {
        return entry["kind"] == "human";
    });
    ASSERT_NE(human, entries.end());

    for (const auto* method : {"audio.start", "audio.startBatch"}) {
        auto params = session;
        params["vault_name"] = "Test";
        if (std::string_view(method) == "audio.start") {
            params["entry_id"] = (*human)["id"];
            params["reference_id"] = "voice-ref";
        } else {
            params["entries"] = nlohmann::json::array({
                {{"entry_id", (*human)["id"]}, {"reference_id", "voice-ref"}},
            });
        }
        const auto reply = call(method, std::move(params));
        EXPECT_FALSE(reply["ok"]) << method;
        EXPECT_EQ(reply["error"]["code"], "invalid_argument") << method;
        EXPECT_EQ(reply["error"]["message"],
            "Voice output is only available for character replies.") << method;
    }
}

TEST_F(BridgeRouterTest, InvalidVoiceProvidersReturnInvalidArgument) {
    bootstrap_epoch();
    const auto voice = call("voice.create", {{"display_name", "Reader"}, {"description", ""},
        {"elevenlabs_voice_id", "voice"}});
    ASSERT_TRUE(voice["ok"]);
    for (const nlohmann::json& provider : {nlohmann::json(1), nlohmann::json(nullptr), nlohmann::json("eleven")}) {
        const auto create = call("voice.create", {{"display_name", "Other"}, {"description", ""},
            {"elevenlabs_voice_id", "voice"}, {"provider", provider}});
        EXPECT_FALSE(create["ok"]);
        EXPECT_EQ(create["error"]["code"], "invalid_argument");
        const auto update = call("voice.update", {{"voice_id", voice["result"]["id"]},
            {"display_name", "Reader"}, {"description", ""}, {"elevenlabs_voice_id", "voice"},
            {"speed", nullptr}, {"provider", provider}});
        EXPECT_FALSE(update["ok"]);
        EXPECT_EQ(update["error"]["code"], "invalid_argument");
        const auto speech = call("speech.start", {{"text", "Hello"}, {"provider", provider}});
        EXPECT_FALSE(speech["ok"]);
        EXPECT_EQ(speech["error"]["code"], "invalid_argument");
        EXPECT_EQ(speech["error"]["message"].get<std::string>().find("FishAudio"), std::string::npos);
    }
}

TEST_F(BridgeRouterTest, SpeechAndAudioMethodsUseOpaqueResources) {
    MockHttpServer server({http_response("audio/mpeg", "AUDIO")});
    server.start();
    bootstrap_epoch();
    const auto key = call("apiKey.create", {
        {"display_name", "Fish"}, {"value", "fish-secret"},
    });
    ASSERT_TRUE(key["ok"]);
    const auto voice = call("voice.create", {
        {"display_name", "Narrator"},
        {"description", "Test"},
        {"elevenlabs_voice_id", "voice-ref"},
    });
    ASSERT_TRUE(voice["ok"]);
    const auto saved = call("voiceOutput.save", {
        {"url", "https://api.fish.audio/v1/tts"},
        {"model", "s2.1-pro"},
        {"api_key", key["result"]["id"]},
        {"output_format", "mp3"},
        {"default_voice", "Narrator"},
    });
    ASSERT_TRUE(saved["ok"]);
    application_->set_speech_url_override(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts");

    const auto started = call("speech.start", {
        {"text", "Hello"}, {"reference_id", "voice-ref"},
    });
    ASSERT_TRUE(started["ok"]) << started.dump();
    EXPECT_TRUE(started["result"]["url"].get<std::string>().starts_with("/media/"));
    const auto resource_id = started["result"]["resource_id"].get<std::string>();
    const auto body = application_->read_resource_chunk(connection_, resource_id, 0);
    ASSERT_TRUE(body);
    EXPECT_EQ(body->body, "AUDIO");

    const auto released = call("speech.release", {{"resource_id", resource_id}});
    EXPECT_TRUE(released["ok"]);
    EXPECT_FALSE(application_->read_resource(connection_, resource_id));

    const auto missing = call("audio.source", {
        {"forum_id", "lobby"},
        {"session_id", "welcome"},
        {"entry_id", 1},
        {"vault_name", "Test"},
    });
    EXPECT_FALSE(missing["ok"]);
    EXPECT_EQ(missing["error"]["code"], "not_found");

    const auto cancelled = call("speech.cancel", {{"request_id", 1}});
    EXPECT_TRUE(cancelled["ok"]);
    server.join();
}

TEST_F(BridgeRouterTest, StaleOauthCompletionDoesNotPublishAfterContextChange) {
    bootstrap_epoch();
    const auto old_epoch = epoch_;
    const auto start_id = next_id_++;
    router_->handle_request(
        connection_,
        request_json(connection_, start_id, old_epoch, "openaiAuth.start").dump());
    router_->run_tasks();
    application_->request_shutdown();
    EXPECT_TRUE(application_->join_shutdown(2s));
    auto batch = wait_delivery(*router_, connection_, 500ms);
    if (batch) {
        auto reply = reply_with_id(*batch, start_id);
        if (!reply.empty() && reply.contains("ok") && !reply["ok"].get<bool>()) {
            EXPECT_EQ(reply["error"]["code"], "application_unavailable");
        }
        ack_delivery(*router_, connection_, *batch);
    }
}

void expect_note_error(
    const nlohmann::json& reply,
    std::string_view code,
    std::string_view message) {
    ASSERT_TRUE(reply.contains("ok"));
    EXPECT_FALSE(reply.at("ok").get<bool>());
    EXPECT_EQ(reply.at("error").at("code").get<std::string>(), code);
    EXPECT_EQ(reply.at("error").at("message").get<std::string>(), message);
}

std::string stored_note_content(
    const std::filesystem::path& path,
    std::int64_t id) {
    storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_only);
    return note_get(database, id).document.dump();
}

nlohmann::json note_byte_fixture() {
    std::ifstream input(
        std::filesystem::path(CHA_SOURCE_DIRECTORY)
        / "tests/fixtures/notes/document-byte-limits.json");
    EXPECT_TRUE(input);
    return nlohmann::json::parse(input);
}

nlohmann::json padded_note_document(const nlohmann::json& base, std::size_t count) {
    nlohmann::json document = base;
    document["content"][0]["content"][0]["text"] = std::string(count, 'a');
    return document;
}

void configure_compression(app::Application& application, int port,
    std::string_view api, bool stream, std::uint64_t epoch,
    int timeout_s = 600, int idle_timeout_s = 60) {
    nlohmann::json provider = application.get_provider("test", epoch);
    for (const char* key : {"id", "used_by", "writable"}) provider.erase(key);
    provider["host"] = "127.0.0.1";
    provider["port"] = port;
    provider["https"] = false;
    provider["mode"] = "net";
    provider["api"] = api;
    provider["stream"] = stream;
    provider["reasoning_effort"] = "low";
    provider["timeout_s"] = timeout_s;
    provider["idle_timeout_s"] = idle_timeout_s;
    (void)application.update_provider("test", provider, epoch);
    (void)application.save_notes_settings({"test"}, epoch);
}

nlohmann::json text_document(std::string_view text) {
    return {{"type", "doc"}, {"content", nlohmann::json::array({{{"type", "paragraph"},
        {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}}})}};
}

NoteItem compression_source(app::Application& application, std::uint64_t epoch) {
    const auto note = application.create_note_item({}, NoteItemKind::note, "Source", epoch);
    application.save_note(note.id, text_document("Original source" + std::string(4096, 'a')), epoch);
    return note;
}

// Headers without a body: the provider request stays open until the client closes it.
const std::string blocked_compression_response =
    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n";

// Queues a request without running it, so a test controls its order against other requests.
std::uint64_t queue_request(BridgeRouter& router, const std::string& connection,
    std::uint64_t& next_id, std::uint64_t epoch, std::string_view method, nlohmann::json params) {
    const auto id = next_id++;
    router.handle_request(connection, request_json(connection, id, epoch, method, std::move(params)).dump());
    return id;
}

// Takes deliveries until every listed reply arrives; one delivery can hold several replies.
std::map<std::uint64_t, nlohmann::json> wait_replies(
    BridgeRouter& router, const std::string& connection, const std::vector<std::uint64_t>& ids) {
    std::map<std::uint64_t, nlohmann::json> replies;
    while (replies.size() < ids.size()) {
        const auto batch = wait_delivery(router, connection, 5s);
        if (!batch) break;
        ack_delivery(router, connection, *batch);
        for (const auto id : ids) {
            if (auto reply = reply_with_id(*batch, id); !reply.empty()) replies[id] = std::move(reply);
        }
    }
    return replies;
}

std::string compression_response(std::string_view api, bool stream,
    std::string_view text, bool incomplete = false) {
    using Json = nlohmann::json;
    if (api == "chat_completions") {
        const Json answer{{"content", text}, {"reasoning_content", "Private reasoning"}};
        if (!stream) return http_response("application/json", Json{{"choices", Json::array({{
            {"message", answer}, {"finish_reason", incomplete ? "length" : "stop"},
        }})}}.dump());
        return http_response("text/event-stream",
            "data: " + Json{{"choices", Json::array({{{"delta", answer}}})}}.dump()
            + "\n\ndata: " + Json{{"choices", Json::array({{
                {"delta", Json::object()}, {"finish_reason", incomplete ? "length" : "stop"},
            }})}}.dump() + "\n\ndata: [DONE]\n\n");
    }
    const Json response{{"status", incomplete ? "incomplete" : "completed"},
        {"incomplete_details", {{"reason", "max_output_tokens"}}},
        {"output", Json::array({{{"type", "message"}, {"content", Json::array({{
            {"type", "output_text"}, {"text", text},
        }})}}})}};
    if (!stream) return http_response("application/json", response.dump());
    return http_response("text/event-stream", "data: " + Json{
        {"type", "response.output_text.delta"}, {"delta", text},
    }.dump() + "\n\ndata: " + Json{
        {"type", incomplete ? "response.incomplete" : "response.completed"}, {"response", response},
    }.dump() + "\n\n");
}

std::string stored_compression(const std::filesystem::path& path, std::int64_t id) {
    storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_only);
    auto statement = database.prepare("SELECT compressed_text FROM note_items WHERE id = ?1", id);
    if (!statement.step() || statement.is_null(0)) return {};
    return statement.text(0);
}

std::string compression_refusal(std::string_view api, bool stream, bool partial_answer) {
    using Json = nlohmann::json;
    if (api == "chat_completions") {
        const Json message{{"content", partial_answer ? Json("Partial compression") : Json(nullptr)},
            {"refusal", "I cannot help"}};
        if (!stream) return http_response("application/json", Json{{"choices", Json::array({{
            {"message", message}, {"finish_reason", "stop"},
        }})}}.dump());
        return http_response("text/event-stream", "data: "
            + Json{{"choices", Json::array({{{"delta", message}}})}}.dump()
            + "\n\ndata: {\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n");
    }
    Json content = Json::array();
    if (partial_answer) content.push_back({{"type", "output_text"}, {"text", "Partial compression"}});
    content.push_back({{"type", "refusal"}, {"refusal", "I cannot help"}});
    const Json response{{"status", "completed"}, {"output", Json::array({{
        {"type", "message"}, {"role", "assistant"}, {"content", content},
    }})}};
    if (!stream) return http_response("application/json", response.dump());
    return http_response("text/event-stream", std::string(partial_answer
        ? "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Partial compression\"}\n\n" : "")
        + "data: {\"type\":\"response.refusal.delta\",\"delta\":\"I cannot help\"}\n\n"
        + "data: " + Json{{"type", "response.completed"}, {"response", response}}.dump() + "\n\n");
}

TEST_F(BridgeRouterTest, YoutubeNoteImportsUseTranscriptPromptAndCompressionSettings) {
    bootstrap_epoch();
    std::ifstream prompt_file(std::filesystem::path(CHA_SOURCE_DIRECTORY) / "resources/youtube-transcript.md");
    const std::string prompt{std::istreambuf_iterator<char>(prompt_file), std::istreambuf_iterator<char>()};
    ASSERT_FALSE(prompt.empty());
    const std::vector<std::pair<std::string, std::string>> urls{
        {"https://www.youtube.com/watch?v=video", "https://www.youtube.com/watch?v=video"},
        {"https://youtu.be/video", "https://www.youtube.com/watch?v=video"},
        {"https://M.YouTube.com/shorts/video", "https://www.youtube.com/shorts/video"},
        {"https://example.org/redirect", "https://www.youtube.com/watch?v=video"},
    };
    std::size_t example = 0;
    for (const auto api : {"chat_completions", "responses"}) {
        for (const bool stream : {false, true}) {
            const auto& [url, final_url] = urls[example++];
            const std::string effort = stream ? "xhigh" : "none";
            SCOPED_TRACE(std::string(api) + " " + url);
            const nlohmann::json page{{"url", final_url}, {"title", "Video title"},
                {"markdown", "00:00 um this is a raw transcript and here is another idea"}};
            MockHttpServer reader({http_response("application/json", page.dump())});
            MockHttpServer provider({compression_response(api, stream,
                "This is a readable transcript.\n\nHere is another idea.")});
            reader.start();
            provider.start();
            configure_compression(*application_, provider.port(), api, stream, epoch_);
            ASSERT_TRUE(call("notesSettings.save", {{"compression_provider", "test"},
                {"compression_reasoning_effort", effort}})["ok"]);
            auto settings = call("webSearch.get")["result"];
            settings["web_reader_enabled"] = true;
            settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
            ASSERT_TRUE(call("webSearch.save", settings)["ok"]);

            const auto reply = call("notes.readPage", {{"url", url}});
            reader.join();
            provider.join();
            ASSERT_TRUE(reply["ok"]) << reply;
            EXPECT_EQ(reply["result"], (nlohmann::json{{"url", final_url}, {"title", "Video title"},
                {"markdown", "# Video title\n\nThis is a readable transcript.\n\nHere is another idea."}}));
            ASSERT_EQ(provider.requests().size(), 1U);
            const auto body = nlohmann::json::parse(request_body(provider.requests().front()));
            EXPECT_FALSE(body.contains("tools"));
            EXPECT_NE(body.dump().find("raw transcript"), std::string::npos);
            EXPECT_EQ(body.dump().find("COMPRESSION RULES"), std::string::npos);
            if (std::string_view(api) == "responses") {
                EXPECT_EQ(body["reasoning"]["effort"], effort);
                EXPECT_NE(body.dump().find(nlohmann::json(prompt).dump()), std::string::npos);
            } else {
                EXPECT_EQ(body["reasoning_effort"], effort);
                EXPECT_EQ(body["messages"][0]["content"], prompt);
                EXPECT_EQ(body["messages"][1]["content"], "from User:\n# Video title\n\n" + page["markdown"].get<std::string>());
            }
            EXPECT_EQ(application_->get_provider("test", epoch_).reasoning_effort, "low");
        }
    }
    EXPECT_TRUE(call("notes.list")["result"]["items"].empty());
}

TEST_F(BridgeRouterTest, YoutubeNoteImportsKeepTitlesThroughFormatting) {
    bootstrap_epoch();
    struct Example {
        std::string title;
        std::string transcript;
        std::string formatted;
        std::string expected_title;
    };
    for (const auto& example : std::vector<Example>{
            {"", "# Original video title\n\nRaw transcript", "Edited transcript", "Original video title"},
            {"", "Raw transcript", "# Generated title\n\nEdited transcript", "Generated title"},
            {"Video title", "Raw transcript", "# Video title\n\nEdited transcript", "Video title"},
            {"Video title", "Raw transcript", "# Changed title\n\nEdited transcript", "Video title"},
            {" \t", "Raw transcript", "\n# Generated title\r\n\r\nEdited transcript", "Generated title"},
        }) {
        SCOPED_TRACE(example.formatted);
        const nlohmann::json page{{"url", "https://youtu.be/video"}, {"title", example.title},
            {"markdown", example.transcript}};
        MockHttpServer reader({http_response("application/json", page.dump())});
        MockHttpServer provider({compression_response("chat_completions", false, example.formatted)});
        reader.start();
        provider.start();
        configure_compression(*application_, provider.port(), "chat_completions", false, epoch_);
        auto settings = call("webSearch.get")["result"];
        settings["web_reader_enabled"] = true;
        settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
        ASSERT_TRUE(call("webSearch.save", settings)["ok"]);

        const auto reply = call("notes.readPage", {{"url", "https://youtu.be/video"}});
        reader.join();
        provider.join();
        ASSERT_TRUE(reply["ok"]) << reply;
        EXPECT_EQ(reply["result"]["title"], example.expected_title);
        EXPECT_EQ(reply["result"]["markdown"], "# " + example.expected_title + "\n\nEdited transcript");
    }
}

TEST_F(BridgeRouterTest, NoteImportsDoNotFormatShortArticlesOrYoutubeLookalikes) {
    bootstrap_epoch();
    for (const auto url : {"https://example.org/youtube.com", "https://youtube.com.example.org/watch",
            "https://notyoutube.com/watch", "https://example.org/?url=https://youtu.be/video"}) {
        SCOPED_TRACE(url);
        const nlohmann::json page{{"url", url}, {"title", "Article"}, {"markdown", "Original article"}};
        MockHttpServer reader({http_response("application/json", page.dump())});
        reader.start();
        auto settings = call("webSearch.get")["result"];
        settings["web_reader_enabled"] = true;
        settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
        ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
        const auto reply = call("notes.readPage", {{"url", url}});
        reader.join();
        ASSERT_TRUE(reply["ok"]) << reply;
        EXPECT_EQ(reply["result"], page);
    }
}

TEST_F(BridgeRouterTest, LongPageParagraphsUseConservativeFormattingAndKeepTheirTitle) {
    bootstrap_epoch();
    std::ifstream prompt_file(std::filesystem::path(CHA_SOURCE_DIRECTORY) / "resources/page-formatting.md");
    const std::string prompt{std::istreambuf_iterator<char>(prompt_file), std::istreambuf_iterator<char>()};
    ASSERT_FALSE(prompt.empty());
    std::string unicode;
    for (int i = 0; i < 2001; ++i) unicode += "é";
    for (const auto& markdown : {std::string(2001, 'a'), std::string(1000, 'a') + "\n" + std::string(1000, 'b'), unicode}) {
        const nlohmann::json page{{"url", "https://example.org/article"}, {"title", "Original title"},
            {"markdown", markdown}};
        const std::string formatted = std::string(1000, 'a') + ".\n\n[More](https://example.org/details).";
        MockHttpServer reader({http_response("application/json", page.dump())});
        MockHttpServer provider({compression_response("chat_completions", false, "# Changed title\n\n" + formatted)});
        reader.start();
        provider.start();
        configure_compression(*application_, provider.port(), "chat_completions", false, epoch_);
        auto settings = call("webSearch.get")["result"];
        settings["web_reader_enabled"] = true;
        settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
        ASSERT_TRUE(call("webSearch.save", settings)["ok"]);

        const auto reply = call("notes.readPage", {{"url", page["url"]}});
        reader.join();
        provider.join();
        ASSERT_TRUE(reply["ok"]) << reply;
        EXPECT_EQ(reply["result"]["title"], "Original title");
        EXPECT_EQ(reply["result"]["markdown"], "# Original title\n\n" + formatted);
        ASSERT_EQ(provider.requests().size(), 1U);
        const auto body = nlohmann::json::parse(request_body(provider.requests().front()));
        EXPECT_EQ(body["messages"][0]["content"], prompt);
        EXPECT_EQ(body["messages"][1]["content"], "from User:\n# Original title\n\n" + markdown);
        EXPECT_FALSE(body.contains("tools"));
    }
}

TEST_F(BridgeRouterTest, PageFormattingIgnoresShortParagraphsHeadingsListsAndCode) {
    bootstrap_epoch();
    const std::string long_text(2001, 'a');
    std::string unicode;
    for (int i = 0; i < 2000; ++i) unicode += "é";
    for (const auto& markdown : {std::string(2000, 'a'), unicode,
            std::string(1500, 'a') + "\r\n \t\r\n" + std::string(1500, 'b'),
            "# " + long_text + "\n\nShort body.", long_text + "\n===\n\nShort body.",
            "- " + long_text + "\n  continued list text", "1. " + long_text,
            "    " + long_text, "\t" + long_text,
            "```text\n\n" + long_text + "\n```\n\nShort body.",
            "~~~~\n```\n\n" + long_text + "\n~~~~"}) {
        SCOPED_TRACE(markdown.substr(0, 40));
        const nlohmann::json page{{"url", "https://example.org/article"}, {"title", "Article"}, {"markdown", markdown}};
        MockHttpServer reader({http_response("application/json", page.dump())});
        reader.start();
        auto settings = call("webSearch.get")["result"];
        settings["web_reader_enabled"] = true;
        settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
        ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
        // No LLM provider is configured: these imports must succeed without one.
        const auto reply = call("notes.readPage", {{"url", page["url"]}});
        reader.join();
        ASSERT_TRUE(reply["ok"]) << reply;
        EXPECT_EQ(reply["result"], page);
    }
}

TEST_F(BridgeRouterTest, YoutubeNoteImportsReportMissingCompressionProvider) {
    bootstrap_epoch();
    const nlohmann::json page{{"url", "https://youtu.be/video"}, {"title", "Video"}, {"markdown", "Transcript"}};
    MockHttpServer reader({http_response("application/json", page.dump())});
    reader.start();
    auto settings = call("webSearch.get")["result"];
    settings["web_reader_enabled"] = true;
    settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    expect_note_error(call("notes.readPage", {{"url", "https://youtu.be/video"}}), "invalid_argument",
        "Select a compression provider in Settings → Notes.");
    reader.join();
}

TEST_F(BridgeRouterTest, YoutubeNoteImportsRejectFailedOrIncompleteFormatting) {
    bootstrap_epoch();
    const nlohmann::json page{{"url", "https://youtu.be/video"}, {"title", "Video"}, {"markdown", "Transcript"}};
    for (const auto api : {"chat_completions", "responses"}) {
        for (const bool stream : {false, true}) {
            SCOPED_TRACE(std::string(api) + (stream ? " streaming" : " non-streaming"));
            const auto reader_response = http_response("application/json", page.dump());
            MockHttpServer reader({reader_response, reader_response, reader_response, reader_response});
            MockHttpServer provider({compression_response(api, stream, "Partial answer", true),
                compression_refusal(api, stream, true), compression_response(api, stream, " \n "),
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
            reader.start();
            provider.start();
            configure_compression(*application_, provider.port(), api, stream, epoch_);
            auto settings = call("webSearch.get")["result"];
            settings["web_reader_enabled"] = true;
            settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
            ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
            std::string incomplete = "The transcript is too long to format.";
            if (std::string_view(api) == "responses") {
                incomplete = stream ? "Responses stream ended incomplete: max_output_tokens"
                    : "Responses body status was incomplete: max_output_tokens";
            }
            const std::vector<std::string> messages{incomplete,
                "The provider refused to format this transcript.",
                "Could not format the YouTube transcript. Try again.",
                "Inference server returned HTTP 503: unknown server error"};
            for (const auto& message : messages) {
                const auto reply = call("notes.readPage", {{"url", "https://youtu.be/video"}});
                expect_note_error(reply, "internal_error", message);
                EXPECT_FALSE(reply.contains("result"));
            }
            reader.join();
            provider.join();
        }
    }
}

TEST_F(BridgeRouterTest, YoutubeNoteImportsDoNotStartFormattingAfterAVaultChange) {
    bootstrap_epoch();
    const nlohmann::json page{{"url", "https://youtu.be/video"}, {"title", "Video"}, {"markdown", "Transcript"}};
    MockHttpServer reader({http_response("application/json", page.dump())});
    reader.pause_before_response(1);
    reader.start();
    // The provider never answers, so a provider call would fail at the 1 s limit.
    MockHttpServer provider({});
    configure_compression(*application_, provider.port(), "chat_completions", false, epoch_, 1);
    auto settings = call("webSearch.get")["result"];
    settings["web_reader_enabled"] = true;
    settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    const auto copy = application_->create_vault(VaultCreate{
        .display_name = "Copied",
        .copy_from = "Test",
        .password = {},
    }, epoch_);
    const auto read = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.readPage", {{"url", "https://youtu.be/video"}});
    router_->run_ordinary_tasks();
    ASSERT_TRUE(reader.wait_for_requests(1, 2s));
    (void)application_->switch_vault(copy.name, {}, epoch_);
    reader.resume_responses();
    auto replies = wait_replies(*router_, connection_, {read});
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[read]["error"]["code"], "vault_changed");
    reader.join();
}

TEST_F(BridgeRouterTest, YoutubeNoteImportsKeepTheProviderTimeout) {
    bootstrap_epoch();
    const nlohmann::json page{{"url", "https://youtu.be/video"}, {"title", "Video"}, {"markdown", "Transcript"}};
    MockHttpServer reader({http_response("application/json", page.dump())});
    // Without Content-Length, the body ends when the server closes, after the saved 1 s limit.
    MockHttpServer slow({"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
        + nlohmann::json{{"choices", nlohmann::json::array({{
            {"message", {{"content", "Slow transcript"}}}, {"finish_reason", "stop"},
        }})}}.dump()}, false, 1200ms);
    reader.start();
    slow.start();
    configure_compression(*application_, slow.port(), "chat_completions", false, epoch_, 1, 2);
    auto settings = call("webSearch.get")["result"];
    settings["web_reader_enabled"] = true;
    settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    expect_note_error(call("notes.readPage", {{"url", "https://youtu.be/video"}}),
        "internal_error", "HTTP request failed: Timeout was reached");
    reader.join();
    slow.join();
}

TEST_F(BridgeRouterTest, CompressionRefusalsPreserveSavedTextAcrossProviderApis) {
    bootstrap_epoch();
    for (const auto api : {"chat_completions", "responses"}) {
        for (const bool stream : {false, true}) {
            SCOPED_TRACE(std::string(api) + (stream ? " streaming" : " non-streaming"));
            MockHttpServer server({compression_refusal(api, stream, false),
                compression_response(api, stream, "Saved compression"),
                compression_refusal(api, stream, false), compression_refusal(api, stream, true),
                compression_response(api, stream, "Successful replacement")});
            server.start();
            configure_compression(*application_, server.port(), api, stream, epoch_);
            const auto note = application_->create_note_item({}, NoteItemKind::note,
                std::string(api) + (stream ? " streaming" : " non-streaming"), epoch_);
            const auto original = text_document("Original source" + std::string(4096, 'a'));
            application_->save_note(note.id, original, epoch_);
            expect_note_error(call("notes.compress", {{"id", note.id}}), "internal_error",
                "The provider refused to compress this note.");
            EXPECT_FALSE(application_->list_note_items(epoch_).back().has_compressed);
            ASSERT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
            for (int refusal = 0; refusal < 2; ++refusal) {
                expect_note_error(call("notes.compress", {{"id", note.id}}), "internal_error",
                    "The provider refused to compress this note.");
                EXPECT_EQ(stored_compression(database_, note.id), "Saved compression");
                EXPECT_TRUE(application_->list_note_items(epoch_).back().has_compressed);
            }
            EXPECT_EQ(application_->get_note(note.id, epoch_).document, original);
            EXPECT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
            EXPECT_EQ(stored_compression(database_, note.id), "Successful replacement");
            server.join();
        }
    }
}

TEST_F(BridgeRouterTest, CompressesOriginalAndReplacesOnlyCompleteAnswersAcrossProviderApis) {
    bootstrap_epoch();
    for (const auto api : {"chat_completions", "responses"}) {
        for (const bool stream : {false, true}) {
            SCOPED_TRACE(std::string(api) + (stream ? " streaming" : " single response"));
            MockHttpServer server({compression_response(api, stream, "First compression"),
                compression_response(api, stream, "Updated compression"),
                compression_response(api, stream, "Partial answer", true),
                compression_response(api, stream, " \n ")});
            server.start();
            configure_compression(*application_, server.port(), api, stream, epoch_);
            const auto note = application_->create_note_item({}, NoteItemKind::note,
                std::string(api) + (stream ? " stream" : " single"), epoch_);
            const auto original = text_document("Original technical content" + std::string(4096, 'a'));
            application_->save_note(note.id, original, epoch_);
            EXPECT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
            EXPECT_EQ(stored_compression(database_, note.id), "First compression");
            EXPECT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
            EXPECT_EQ(stored_compression(database_, note.id), "Updated compression");
            EXPECT_FALSE(call("notes.compress", {{"id", note.id}})["ok"]);
            EXPECT_FALSE(call("notes.compress", {{"id", note.id}})["ok"]);
            EXPECT_EQ(stored_compression(database_, note.id), "Updated compression");
            EXPECT_EQ(application_->get_note(note.id, epoch_).document, original);
            EXPECT_TRUE(application_->rename_note_item(note.id, note.name + " renamed", epoch_).has_compressed);
            EXPECT_EQ(application_->get_provider("test", epoch_).reasoning_effort, "low");
            server.join();
            ASSERT_EQ(server.requests().size(), 4U);
            for (const auto& raw : server.requests()) {
                const auto body = nlohmann::json::parse(request_body(raw));
                EXPECT_FALSE(body.contains("tools"));
                EXPECT_NE(body.dump().find("Original technical content"), std::string::npos);
                EXPECT_EQ(body.dump().find("Updated compression"), std::string::npos);
                EXPECT_NE(body.dump().find("COMPRESSION RULES"), std::string::npos);
                if (std::string_view(api) == "responses") EXPECT_EQ(body["reasoning"]["effort"], "high");
                else EXPECT_EQ(body["reasoning_effort"], "high");
            }
        }
    }
}

TEST_F(BridgeRouterTest, CompressionUsesSelectedEffortAcrossProviderApis) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    for (const auto api : {"chat_completions", "responses"}) {
        for (const auto effort : {"none", "low", "medium", "high", "xhigh"}) {
            SCOPED_TRACE(std::string(api) + " " + effort);
            MockHttpServer server({compression_response(api, false, "Compressed text")});
            server.start();
            configure_compression(*application_, server.port(), api, false, epoch_);
            ASSERT_TRUE(call("notesSettings.save", {{"compression_provider", "test"},
                {"compression_reasoning_effort", effort}})["ok"]);
            ASSERT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
            server.join();
            ASSERT_EQ(server.requests().size(), 1U);
            const auto body = nlohmann::json::parse(request_body(server.requests().front()));
            if (std::string_view(api) == "responses") EXPECT_EQ(body["reasoning"]["effort"], effort);
            else EXPECT_EQ(body["reasoning_effort"], effort);
            EXPECT_EQ(application_->get_provider("test", epoch_).reasoning_effort, "low");
        }
    }
}

TEST_F(BridgeRouterTest, CompressionSkipsShortMarkdownBeforeProviderValidation) {
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Source", epoch_);
    expect_note_error(call("notes.compress", {{"id", note.id}}), "invalid_argument",
        "This note is under 4 KiB and does not need compression.");
    const auto short_document = text_document(std::string(4095, 'a'));
    ASSERT_GT(short_document.dump().size(), 4096U);
    application_->save_note(note.id, short_document, epoch_);
    expect_note_error(call("notes.compress", {{"id", note.id}}), "invalid_argument",
        "This note is under 4 KiB and does not need compression.");

    MockHttpServer server({compression_response("chat_completions", false, "Compressed text")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note.id, "Existing compression");
    }
    expect_note_error(call("notes.compress", {{"id", note.id}}), "invalid_argument",
        "This note is under 4 KiB and does not need compression.");
    EXPECT_EQ(stored_compression(database_, note.id), "Existing compression");
    EXPECT_EQ(application_->get_note(note.id, epoch_).document, short_document);

    // Count UTF-8 bytes, not characters, and allow exactly 4 KiB.
    std::string text;
    for (int i = 0; i < 2048; ++i) text += "é";
    ASSERT_EQ(text.size(), 4096U);
    application_->save_note(note.id, text_document(text), epoch_);
    ASSERT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
    server.join();
    ASSERT_EQ(server.requests().size(), 1U);
    EXPECT_EQ(stored_compression(database_, note.id), "Compressed text");
}

TEST_F(BridgeRouterTest, FolderCompressionSkipsStoredAndShortNotesBeforeProviderValidation) {
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Source", epoch_);
    const auto compress = [&] { return call("notes.compress", {{"id", note.id}, {"skip_ineligible", true}}); };
    auto reply = compress();
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"], nlohmann::json({{"skipped", "too_small"}}));
    application_->save_note(note.id, text_document(std::string(4095, 'a')), epoch_);
    reply = compress();
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["skipped"], "too_small");
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note.id, "Existing compression");
    }
    reply = compress();
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["skipped"], "already_compressed");
    application_->save_note(note.id, text_document(std::string(4096, 'a')), epoch_);
    reply = compress();
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["skipped"], "already_compressed");
    EXPECT_EQ(stored_compression(database_, note.id), "Existing compression");

    const auto eligible = application_->create_note_item({}, NoteItemKind::note, "Eligible", epoch_);
    application_->save_note(eligible.id, text_document(std::string(4096, 'a')), epoch_);
    MockHttpServer server({compression_response("chat_completions", false, "New compression")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    reply = call("notes.compress", {{"id", eligible.id}, {"skip_ineligible", true}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"], nlohmann::json::object());
    EXPECT_EQ(stored_compression(database_, eligible.id), "New compression");
    server.join();
    EXPECT_EQ(server.requests().size(), 1U);
    EXPECT_FALSE(call("notes.compress", {{"id", eligible.id}, {"skip_ineligible", "true"}})["ok"]);
}

TEST_F(BridgeRouterTest, CompressionReportsProviderSettingsAndEmptyNotes) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    expect_note_error(call("notes.compress", {{"id", note.id}}), "invalid_argument",
        "Select a compression provider in Settings → Notes.");
    application_->save_note(note.id, text_document(std::string(4096, ' ')), epoch_);
    expect_note_error(call("notes.compress", {{"id", note.id}}), "invalid_argument",
        "This note has no text to compress.");
    application_->save_note(note.id, text_document("Original source" + std::string(4096, 'a')), epoch_);
    (void)application_->save_notes_settings({"test"}, epoch_);
    for (const auto api : {"chat_completions", "responses"}) {
        nlohmann::json provider = application_->get_provider("test", epoch_);
        for (const char* key : {"id", "used_by", "writable"}) provider.erase(key);
        provider["api"] = api;
        provider["host"] = std::string_view(api) == "responses" ? "api.openai.com" : "api.mistral.ai";
        provider["model"] = std::string_view(api) == "responses" ? "gpt-4o" : "mistral-large-latest";
        (void)application_->update_provider("test", provider, epoch_);
        expect_note_error(call("notes.compress", {{"id", note.id}}), "invalid_argument",
            "This compression provider's model does not support high reasoning effort.");
    }
    nlohmann::json provider = application_->get_provider("test", epoch_);
    for (const char* key : {"id", "used_by", "writable"}) provider.erase(key);
    provider["host"] = "127.0.0.1";
    provider["model"] = "compression-model";
    provider["api_key"] = "missing-key";
    (void)application_->update_provider("test", provider, epoch_);
    // The user can fix a missing key, so the reason must reach the dialog.
    expect_note_error(call("notes.compress", {{"id", note.id}}), "invalid_argument",
        "API key 'missing-key' does not exist");
}

TEST_F(BridgeRouterTest, CompressionCancelDuringTheRequestKeepsTheOldText) {
    bootstrap_epoch();
    EXPECT_FALSE(is_control_method(Method::notes_compress_cancel));
    MockHttpServer server({compression_response("chat_completions", false, "Old compression"),
        blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
    const auto compress = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
    router_->run_ordinary_tasks();
    ASSERT_TRUE(server.wait_for_requests(2, 2s));
    const auto cancel = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compressCancel", {{"request_id", compress}});
    router_->run_ordinary_tasks();
    auto replies = wait_replies(*router_, connection_, {compress, cancel});
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_TRUE(replies[cancel]["ok"]);
    EXPECT_EQ(replies[compress]["error"]["code"], "operation_cancelled");
    EXPECT_EQ(stored_compression(database_, note.id), "Old compression");
    EXPECT_NO_THROW(server.join());
}

TEST_F(BridgeRouterTest, CompressionCommittedBeforeCancelStaysSaved) {
    bootstrap_epoch();
    MockHttpServer server({compression_response("chat_completions", false, "Old compression"),
        compression_response("chat_completions", false, "New compression")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
    const auto compress = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
    router_->run_ordinary_tasks();
    // Wait for the commit, but do not take the reply: Cancel must arrive before its delivery.
    for (const auto deadline = std::chrono::steady_clock::now() + 5s;
         stored_compression(database_, note.id) != "New compression"
         && std::chrono::steady_clock::now() < deadline;) {
        router_->wait_for_work(5ms);
    }
    ASSERT_EQ(stored_compression(database_, note.id), "New compression");
    const auto cancel = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compressCancel", {{"request_id", compress}});
    router_->run_ordinary_tasks();
    auto replies = wait_replies(*router_, connection_, {compress, cancel});
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_TRUE(replies[cancel]["ok"]);
    EXPECT_TRUE(replies[compress]["ok"]);
    EXPECT_EQ(stored_compression(database_, note.id), "New compression");
    server.join();
}

TEST_F(BridgeRouterTest, CompressionCancelQueuedBehindStartupIsNotLost) {
    bootstrap_epoch();
    // This listener never answers, so the provider cannot finish before Cancel.
    MockHttpServer silent({});
    configure_compression(*application_, silent.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    const auto compress = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
    const auto cancel = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compressCancel", {{"request_id", compress}});
    router_->run_ordinary_tasks();
    auto replies = wait_replies(*router_, connection_, {compress, cancel});
    ASSERT_EQ(replies.size(), 2U);
    EXPECT_TRUE(replies[cancel]["ok"]);
    EXPECT_EQ(replies[compress]["error"]["code"], "operation_cancelled");
    EXPECT_EQ(stored_compression(database_, note.id), "");
}

TEST_F(BridgeRouterTest, ClosingTheConnectionCancelsCompression) {
    bootstrap_epoch();
    MockHttpServer server({blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    (void)queue_request(*router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
    router_->run_ordinary_tasks();
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    router_->close_connection(connection_);
    // The server returns only after the client has closed the provider request.
    EXPECT_NO_THROW(server.join());
    EXPECT_EQ(stored_compression(database_, note.id), "");
}

TEST_F(BridgeRouterTest, CompressionKeepsAResultAfterAnEditButNotAfterDeletion) {
    bootstrap_epoch();
    MockHttpServer server({compression_response("chat_completions", false, "From the original"),
        compression_response("chat_completions", false, "Too late")});
    server.pause_before_response(1);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    auto compress = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
    router_->run_ordinary_tasks();
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    const auto edited = text_document("Edited source" + std::string(4096, 'a'));
    application_->save_note(note.id, edited, epoch_);
    server.resume_responses();
    EXPECT_TRUE(wait_replies(*router_, connection_, {compress})[compress]["ok"]);
    EXPECT_EQ(stored_compression(database_, note.id), "From the original");
    EXPECT_EQ(application_->get_note(note.id, epoch_).document, edited);

    server.pause_before_response(2);
    compress = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
    router_->run_ordinary_tasks();
    ASSERT_TRUE(server.wait_for_requests(2, 2s));
    application_->delete_note_item(note.id, epoch_);
    server.resume_responses();
    expect_note_error(wait_replies(*router_, connection_, {compress})[compress],
        "not_found", "That note was not found.");
    server.join();
    EXPECT_NE(request_body(server.requests()[0]).find("Original source"), std::string::npos);
}

TEST_F(BridgeRouterTest, CompressionCannotSaveIntoAChangedVault) {
    bootstrap_epoch();
    MockHttpServer server({compression_response("chat_completions", false, "Old compression"),
        blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compress", {{"id", note.id}})["ok"]);
    const auto copy = application_->create_vault(VaultCreate{
        .display_name = "Copied",
        .copy_from = "Test",
        .password = {},
    }, epoch_);
    const auto compress = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
    router_->run_ordinary_tasks();
    ASSERT_TRUE(server.wait_for_requests(2, 2s));
    (void)application_->switch_vault(copy.name, {}, epoch_);
    // The server returns only after the vault change has closed the provider request.
    EXPECT_NO_THROW(server.join());
    auto replies = wait_replies(*router_, connection_, {compress});
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies[compress]["error"]["code"], "vault_changed");
    EXPECT_EQ(stored_compression(database_, note.id), "Old compression");
}

TEST_F(BridgeRouterTest, CompressionHasNoTransferLimitButKeepsTheIdleTimeout) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    {
        // Without Content-Length, the body ends when the server closes, after the saved 1 s limit.
        MockHttpServer slow({"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
            + nlohmann::json{{"choices", nlohmann::json::array({{
                {"message", {{"content", "Slow compression"}}}, {"finish_reason", "stop"},
            }})}}.dump()}, false, 1200ms);
        slow.start();
        configure_compression(*application_, slow.port(), "chat_completions", false, epoch_, 1, 2);
        const auto compress = queue_request(
            *router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
        EXPECT_TRUE(wait_replies(*router_, connection_, {compress})[compress]["ok"]);
        slow.join();
    }
    EXPECT_EQ(stored_compression(database_, note.id), "Slow compression");
    // The idle timer starts at the first body byte.
    MockHttpServer stalled({blocked_compression_response + ": waiting\n\n"}, true);
    stalled.start();
    configure_compression(*application_, stalled.port(), "chat_completions", false, epoch_, 1, 1);
    const auto compress = queue_request(
        *router_, connection_, next_id_, epoch_, "notes.compress", {{"id", note.id}});
    expect_note_error(wait_replies(*router_, connection_, {compress})[compress],
        "internal_error", "HTTP request failed: idle timeout reached");
    EXPECT_NO_THROW(stalled.join());
    EXPECT_EQ(stored_compression(database_, note.id), "Slow compression");
}

TEST_F(BridgeRouterTest, NotesWireContract) {
    bootstrap_epoch();
    const auto empty = call("notes.list");
    ASSERT_TRUE(empty["ok"]);
    EXPECT_TRUE(empty["result"]["items"].empty());

    const auto folder = call("notes.create", {
        {"parent_id", nullptr}, {"kind", "folder"}, {"name", "Plan"},
    });
    ASSERT_TRUE(folder["ok"]);
    EXPECT_EQ(folder["result"]["parent_id"], nullptr);
    EXPECT_EQ(folder["result"]["kind"], "folder");
    EXPECT_EQ(folder["result"]["name"], "Plan");
    EXPECT_EQ(folder["result"].size(), 6);
    EXPECT_EQ(folder["result"]["has_compressed"], false);
    EXPECT_EQ(folder["result"]["read_only"], false);
    const auto folder_id = folder["result"]["id"].get<std::int64_t>();

    const auto note = call("notes.create", {
        {"parent_id", folder_id}, {"kind", "note"}, {"name", "Draft"},
    });
    ASSERT_TRUE(note["ok"]);
    const auto note_id = note["result"]["id"].get<std::int64_t>();
    const auto loaded = call("notes.get", {{"id", note_id}});
    ASSERT_TRUE(loaded["ok"]);
    EXPECT_EQ(loaded["result"]["id"], note_id);
    EXPECT_EQ(
        loaded["result"]["document"],
        nlohmann::json::parse(std::string(empty_note_document)));

    const auto document = nlohmann::json::parse(R"({"type":"doc","extra":true})");
    ASSERT_TRUE(call("notes.save", {{"id", note_id}, {"document", document}})["ok"]);
    ASSERT_TRUE(call("notes.save", {{"id", note_id}, {"document", document}})["ok"]);
    EXPECT_EQ(call("notes.get", {{"id", note_id}})["result"]["document"], document);

    EXPECT_FALSE(call("notes.get", {{"id", note_id}})["result"].contains("compressed_text"));
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note_id, "Stored compressed text");
    }
    const auto compressed = call("notes.get", {{"id", note_id}});
    ASSERT_TRUE(compressed["ok"]);
    EXPECT_EQ(compressed["result"]["compressed_text"], "Stored compressed text");
    EXPECT_EQ(compressed["result"]["document"], document);

    const auto renamed = call("notes.rename", {{"id", note_id}, {"name", "Final"}});
    ASSERT_TRUE(renamed["ok"]);
    EXPECT_EQ(renamed["result"]["name"], "Final");
    expect_note_error(
        call("notes.get", {{"id", folder_id}}),
        "invalid_argument",
        "That item is not a note.");
    ASSERT_TRUE(call("notes.delete", {{"id", note_id}})["ok"]);
    expect_note_error(
        call("notes.get", {{"id", note_id}}),
        "not_found",
        "That note was not found.");
    expect_note_error(
        call("notes.save", {{"id", note_id}, {"document", document}}),
        "not_found",
        "That note was not found.");

    expect_note_error(
        call("notes.create", {
            {"parent_id", nullptr}, {"kind", "folder"}, {"name", "Plan"}, {"extra", true},
        }),
        "invalid_argument",
        "The request was not valid.");
    expect_note_error(
        call("notes.get", {{"id", 0}}),
        "invalid_argument",
        "The item id is not valid.");
    expect_note_error(
        call("notes.get", {{"id", -1}}),
        "invalid_argument",
        "The item id is not valid.");
    expect_note_error(
        call("notes.get", {{"id", 1.5}}),
        "invalid_argument",
        "The item id is not valid.");
    expect_note_error(
        call("notes.get", {{"id", "4"}}),
        "invalid_argument",
        "The item id is not valid.");
    expect_note_error(
        call("notes.get", {{"id", nullptr}}),
        "invalid_argument",
        "The item id is not valid.");
    expect_note_error(
        call("notes.get", {{"id", 9007199254740992ULL}}),
        "invalid_argument",
        "The item id is not valid.");
    expect_note_error(
        call("notes.create", {
            {"parent_id", 0}, {"kind", "note"}, {"name", "Draft"},
        }),
        "invalid_argument",
        "The parent is not valid.");
    expect_note_error(
        call("notes.create", {
            {"parent_id", nullptr}, {"kind", "file"}, {"name", "Draft"},
        }),
        "invalid_argument",
        "Kind must be folder or note.");
    expect_note_error(
        call("notes.create", {
            {"parent_id", nullptr}, {"kind", "note"}, {"name", ""},
        }),
        "invalid_argument",
        "Name must not be empty");
    expect_note_error(
        call("notes.save", {{"id", note_id}, {"document", nlohmann::json::array()}}),
        "invalid_argument",
        "Note content must be a JSON object.");
    expect_note_error(
        call("notes.create", {
            {"parent_id", nullptr}, {"kind", "folder"}, {"name", "Plan"},
        }),
        "invalid_argument",
        "An item with this name already exists here.");

    const auto kept = call("notes.create", {
        {"parent_id", nullptr}, {"kind", "note"}, {"name", "Kept"},
    });
    ASSERT_TRUE(kept["ok"]);
    const auto kept_id = kept["result"]["id"].get<std::int64_t>();
    {
        storage::SqliteDatabase database(
            database_, storage::SqliteDatabase::Mode::read_write);
        auto statement = database.prepare(
            "UPDATE note_items SET content_json = ?1 WHERE id = ?2");
        statement.bind_blob(1, "{");
        statement.bind(2, kept_id);
        statement.run();
    }
    expect_note_error(
        call("notes.get", {{"id", kept_id}}),
        "internal_error",
        "The note could not be opened.");
    {
        storage::SqliteDatabase database(
            database_, storage::SqliteDatabase::Mode::read_write);
        auto statement = database.prepare(
            "UPDATE note_items SET content_json = ?1 WHERE id = ?2");
        statement.bind_blob(1, "[]");
        statement.bind(2, kept_id);
        statement.run();
    }
    expect_note_error(
        call("notes.get", {{"id", kept_id}}),
        "internal_error",
        "The note could not be opened.");
    const auto listed = call("notes.list");
    ASSERT_TRUE(listed["ok"]);
    EXPECT_GE(listed["result"]["items"].size(), 1);
}

TEST_F(BridgeRouterTest, NotesRejectOneByteOverTheDocumentLimit) {
    bootstrap_epoch();
    const auto created = call("notes.create", {
        {"parent_id", nullptr}, {"kind", "note"}, {"name", "Large"},
    });
    ASSERT_TRUE(created["ok"]);
    const auto id = created["result"]["id"].get<std::int64_t>();
    const auto fixture = note_byte_fixture();
    const auto limit = fixture["limit"].get<std::size_t>();
    const auto base = fixture["padding"]["document"];
    const auto overhead = padded_note_document(base, 0).dump().size();
    const auto exact = padded_note_document(base, limit - overhead);
    const auto over = padded_note_document(base, limit - overhead + 1);
    ASSERT_EQ(exact.dump().size(), limit);
    ASSERT_TRUE(call("notes.save", {{"id", id}, {"document", exact}})["ok"]);
    expect_note_error(
        call("notes.save", {{"id", id}, {"document", over}}),
        "body_too_large",
        "Note exceeds 12 MiB. Remove content or use a smaller picture.");
    EXPECT_EQ(stored_note_content(database_, id), exact.dump());
}

TEST_F(BridgeRouterTest, ReadOnlyNoteWireContractAndValidation) {
    bootstrap_epoch();
    const auto note = call("notes.create", {
        {"parent_id", nullptr}, {"kind", "note"}, {"name", "Source"},
    });
    ASSERT_TRUE(note["ok"]);
    const auto id = note["result"]["id"];
    const auto locked = call("notes.setReadOnly", {{"id", id}, {"read_only", true}});
    ASSERT_TRUE(locked["ok"]) << locked.dump();
    EXPECT_EQ(locked["result"]["read_only"], true);
    EXPECT_EQ(call("notes.list")["result"]["items"][0]["read_only"], true);
    const nlohmann::json document = {{"type", "doc"}, {"changed", true}};
    expect_note_error(call("notes.save", {{"id", id}, {"document", document}}),
        "invalid_argument", "This note is read-only. Make it editable first.");
    ASSERT_TRUE(call("notes.setReadOnly", {{"id", id}, {"read_only", false}})["ok"]);
    ASSERT_TRUE(call("notes.save", {{"id", id}, {"document", document}})["ok"]);
    for (const auto& invalid : {nlohmann::json{{"id", id}},
            nlohmann::json{{"id", id}, {"read_only", 1}},
            nlohmann::json{{"id", id}, {"read_only", true}, {"extra", true}}}) {
        EXPECT_FALSE(call("notes.setReadOnly", invalid)["ok"]);
    }
}

TEST_F(BridgeRouterTest, ScrollPositionWireContractAndValidation) {
    bootstrap_epoch();
    const auto note = call("notes.create", {
        {"parent_id", nullptr}, {"kind", "note"}, {"name", "Reading"},
    });
    ASSERT_TRUE(note["ok"]);
    const auto id = note["result"]["id"];
    const auto original = call("notes.get", {{"id", id}})["result"];
    EXPECT_EQ(original["scroll_top"], 0);
    ASSERT_TRUE(call("notes.setReadOnly", {{"id", id}, {"read_only", true}})["ok"]);
    const auto saved = call("notes.saveScrollPosition", {{"id", id}, {"scroll_top", 1200}});
    ASSERT_TRUE(saved["ok"]) << saved.dump();
    EXPECT_TRUE(saved["result"].empty());
    EXPECT_EQ(call("notes.get", {{"id", id}})["result"]["scroll_top"], 1200);
    EXPECT_EQ(call("notes.get", {{"id", id}})["result"]["document"], original["document"]);
    for (const auto& invalid : {nlohmann::json{{"id", id}},
            nlohmann::json{{"id", id}, {"scroll_top", -1}},
            nlohmann::json{{"id", id}, {"scroll_top", 1.5}},
            nlohmann::json{{"id", id}, {"scroll_top", "100"}},
            nlohmann::json{{"id", id}, {"scroll_top", 9007199254740992ULL}},
            nlohmann::json{{"id", id}, {"scroll_top", 0}, {"extra", true}}}) {
        EXPECT_FALSE(call("notes.saveScrollPosition", invalid)["ok"]);
    }
    ASSERT_TRUE(call("notes.saveScrollPosition", {{"id", id}, {"scroll_top", 0}})["ok"]);
    EXPECT_EQ(call("notes.get", {{"id", id}})["result"]["scroll_top"], 0);
}

TEST_F(BridgeRouterTest, BookImportWireContractAndValidation) {
    (void)bootstrap_epoch();
    const auto document = nlohmann::json::parse(empty_note_document);
    const nlohmann::json request = {{"parent_id", nullptr}, {"name", "Book"}, {"notes", {
        {{"name", "001. First"}, {"document", document}},
        {{"name", "002. Second"}, {"document", document}},
    }}};
    const auto imported = call("notes.importBook", request);
    ASSERT_TRUE(imported["ok"]) << imported.dump();
    EXPECT_EQ(imported["result"].size(), 1U);
    const auto folder = imported["result"]["id"].get<std::int64_t>();
    const auto items = call("notes.list")["result"]["items"];
    ASSERT_EQ(items.size(), 3U);
    EXPECT_EQ(items[0]["id"], folder);
    EXPECT_EQ(items[1]["parent_id"], folder);
    EXPECT_EQ(items[1]["read_only"], true);
    EXPECT_EQ(items[2]["read_only"], true);
    EXPECT_EQ(call("notes.get", {{"id", items[1]["id"]}})["result"]["document"], document);
    expect_note_error(call("notes.importBook", request), "invalid_argument",
        "An item with this name already exists here.");
    for (const nlohmann::json& invalid : {
            nlohmann::json{{"parent_id", nullptr}, {"name", "Other"}, {"notes", nlohmann::json::object()}},
            nlohmann::json{{"parent_id", nullptr}, {"name", "Other"}, {"notes", {{{"name", "001. Bad"}, {"document", nullptr}}}}},
            nlohmann::json{{"parent_id", nullptr}, {"name", "Other"}, {"notes", {{{"name", "001. Bad"}, {"document", document}, {"extra", 1}}}}},
            nlohmann::json{{"parent_id", nullptr}, {"name", "Other"}, {"notes", nlohmann::json::array()}},
        }) {
        EXPECT_FALSE(call("notes.importBook", invalid)["ok"]);
    }
    EXPECT_EQ(call("notes.list")["result"]["items"].size(), 3U);
}

TEST_F(BridgeRouterTest, BookImportUsesSelectedFolderAndRejectsInvalidParents) {
    (void)bootstrap_epoch();
    const auto folder = call("notes.create", {
        {"parent_id", nullptr}, {"kind", "folder"}, {"name", "Research"},
    })["result"]["id"];
    const auto document = nlohmann::json::parse(empty_note_document);
    nlohmann::json request = {{"parent_id", folder}, {"name", "Book"}, {"notes", {
        {{"name", "001. First"}, {"document", document}},
    }}};
    const auto imported = call("notes.importBook", request);
    ASSERT_TRUE(imported["ok"]) << imported.dump();
    const auto items = call("notes.list")["result"]["items"];
    ASSERT_EQ(items.size(), 3U);
    EXPECT_EQ(items[1]["id"], imported["result"]["id"]);
    EXPECT_EQ(items[1]["parent_id"], folder);
    EXPECT_EQ(items[2]["parent_id"], imported["result"]["id"]);
    for (const auto& invalid : {nlohmann::json(0), nlohmann::json(-1), nlohmann::json("folder"), items[2]["id"]}) {
        request["parent_id"] = invalid;
        EXPECT_FALSE(call("notes.importBook", request)["ok"]);
    }
    request.erase("parent_id");
    EXPECT_FALSE(call("notes.importBook", request)["ok"]);
    EXPECT_EQ(call("notes.list")["result"]["items"], items);
}


TEST_F(BridgeRouterTest, AnnotationWireContractGuardsAndChatText) {
    bootstrap_epoch();
    const auto source = call("notes.create", {{"parent_id", nullptr}, {"kind", "note"}, {"name", "Source"}})["result"]["id"];
    const auto doc = nlohmann::json::parse(R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Passage","marks":[{"type":"highlight"}]}]}]})");
    const auto saved = call("notes.save", {{"id", source}, {"document", doc}});
    ASSERT_TRUE(saved["ok"]);
    EXPECT_EQ(saved["result"], nlohmann::json({{"annotation_created", true}}));
    EXPECT_EQ(call("notes.save", {{"id", source}, {"document", doc}})["result"], nlohmann::json({{"annotation_created", false}}));
    const auto items = call("notes.list")["result"]["items"];
    ASSERT_EQ(items.size(), 2u);
    const auto id = items[1]["id"];
    EXPECT_EQ(items[1]["kind"], "annotation");
    EXPECT_TRUE(call("notes.get", {{"id", id}})["ok"]);
    EXPECT_TRUE(call("notes.saveScrollPosition", {{"id", id}, {"scroll_top", 17}})["ok"]);
    for (const auto& method : {"notes.compress", "notes.save", "notes.rename", "notes.delete", "notes.setReadOnly"}) {
        auto params = nlohmann::json{{"id", id}};
        if (std::string_view(method) == "notes.save") params["document"] = doc;
        if (std::string_view(method) == "notes.rename") params["name"] = "Other";
        if (std::string_view(method) == "notes.setReadOnly") params["read_only"] = false;
        expect_note_error(call(method, params), "invalid_argument", "That item is not a note.");
    }
    expect_note_error(call("notes.compress", {{"id", id}, {"skip_ineligible", true}}), "invalid_argument", "That item is not a note.");
    EXPECT_FALSE(call("notes.create", {{"parent_id", nullptr}, {"kind", "annotation"}, {"name", "Other"}})["ok"]);
}

TEST_F(BridgeRouterTest, NotesRejectStaleVaultContext) {
    const auto old_epoch = bootstrap_epoch();
    const auto created_note = call("notes.create", {
        {"parent_id", nullptr}, {"kind", "note"}, {"name", "Shared"},
    });
    ASSERT_TRUE(created_note["ok"]);
    const auto id = created_note["result"]["id"].get<std::int64_t>();
    const auto original = nlohmann::json::parse(R"({"type":"doc","text":"original"})");
    ASSERT_TRUE(call("notes.save", {{"id", id}, {"document", original}})["ok"]);
    const auto original_bytes = stored_note_content(database_, id);

    const auto created = application_->create_vault(VaultCreate{
        .display_name = "Copied",
        .copy_from = "Test",
        .password = {},
    }, old_epoch);
    const auto switched = application_->switch_vault(created.name, {}, old_epoch);
    auto notification = wait_delivery(*router_, connection_);
    ASSERT_TRUE(notification);
    ack_delivery(*router_, connection_, *notification);

    epoch_ = switched.context_epoch;
    const auto changed = nlohmann::json::parse(R"({"type":"doc","text":"copy"})");
    ASSERT_TRUE(call("notes.save", {{"id", id}, {"document", changed}})["ok"]);
    epoch_ = old_epoch;
    expect_note_error(call("notes.saveScrollPosition", {{"id", id}, {"scroll_top", 100}}),
        "vault_changed", "The vault context has changed.");
    expect_note_error(call("notes.setReadOnly", {{"id", id}, {"read_only", true}}),
        "vault_changed", "The vault context has changed.");
    expect_note_error(
        call("notes.importBook", {{"parent_id", nullptr}, {"name", "Stale Book"}, {"notes", {
            {{"name", "001. First"}, {"document", original}},
        }}}),
        "vault_changed", "The vault context has changed.");
    expect_note_error(
        call("notes.save", {{"id", id}, {"document", original}}),
        "vault_changed",
        "The vault context has changed.");
    epoch_ = switched.context_epoch;
    EXPECT_EQ(call("notes.get", {{"id", id}})["result"]["document"], changed);
    EXPECT_EQ(stored_note_content(created.data, id), changed.dump());
    EXPECT_EQ(stored_note_content(database_, id), original_bytes);

    const auto back = application_->switch_vault("Test", {}, switched.context_epoch);
    notification = wait_delivery(*router_, connection_);
    ASSERT_TRUE(notification);
    ack_delivery(*router_, connection_, *notification);
    epoch_ = back.context_epoch;
    EXPECT_EQ(call("notes.get", {{"id", id}})["result"]["document"], original);
    EXPECT_EQ(stored_note_content(database_, id), original_bytes);
    EXPECT_EQ(stored_note_content(created.data, id), changed.dump());
}

TEST_F(BridgeRouterTest, NotesAdmissionRejectsMaintenanceAndStopping) {
    const auto old_epoch = bootstrap_epoch();
    const auto created_note = application_->create_note_item(
        std::nullopt, NoteItemKind::note, "Held", old_epoch);
    const auto original = stored_note_content(database_, created_note.id);
    const auto created = application_->create_vault(VaultCreate{
        .display_name = "Copied",
        .copy_from = "Test",
        .password = {},
    }, old_epoch);

    std::mutex mutex;
    std::condition_variable changed;
    bool paused = false;
    bool release = false;
    application_->set_resource_hooks({
        .pause = [&](bool) {
            std::unique_lock lock(mutex);
            paused = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        },
        .resume = [] {},
    });
    auto switching = std::async(std::launch::async, [&] {
        return application_->switch_vault(created.name, {}, old_epoch);
    });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return paused; }));
    }
    auto saving = std::async(std::launch::async, [&] {
        application_->save_note(
            created_note.id,
            nlohmann::json::parse(R"({"type":"doc","text":"rejected"})"),
            old_epoch);
    });
    EXPECT_EQ(saving.wait_for(200ms), std::future_status::timeout);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_EQ(switching.get().state, app::ApplicationState::running);
    auto notification = wait_delivery(*router_, connection_);
    ASSERT_TRUE(notification);
    ack_delivery(*router_, connection_, *notification);
    try {
        saving.get();
        FAIL() << "expected vault_changed";
    } catch (const app::ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::vault_changed);
    }
    EXPECT_EQ(stored_note_content(database_, created_note.id), original);
    EXPECT_EQ(stored_note_content(created.data, created_note.id), original);

    application_->set_resource_hooks({});
    application_->request_shutdown();
    try {
        (void)application_->list_note_items(application_->context_epoch());
        FAIL() << "expected application_unavailable";
    } catch (const app::ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::application_unavailable);
    }
}

TEST_F(BridgeRouterTest, NotesListUsesTheOrdinaryRequestLimit) {
    router_ = std::make_unique<BridgeRouter>(
        *application_,
        BridgeRouter::Options{.platform = "test", .ordinary_limit = 1});
    connection_ = router_->open_connection();
    router_->handle_request(
        connection_, request_json(connection_, 1, 0, "bridge.info").dump());
    router_->run_tasks();
    auto first = router_->take_delivery(connection_);
    ASSERT_TRUE(first);
    router_->handle_request(
        connection_, request_json(connection_, 2, 1, "notes.list").dump());
    EXPECT_FALSE(router_->take_delivery(connection_));
    ack_delivery(*router_, connection_, *first);
    auto overflow = wait_delivery(*router_, connection_);
    ASSERT_TRUE(overflow);
    const auto reply = reply_with_id(*overflow, 2);
    ASSERT_FALSE(reply.empty());
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "command_queue_full");
    EXPECT_EQ(reply["error"]["message"], "Too many in-flight requests.");
    ack_delivery(*router_, connection_, *overflow);
}

} // namespace
} // namespace cha::bridge
