#include "app/application.h"
#include "app/current_vault.h"
#include "bridge/bridge_router.h"
#include "providers/api_key_store.h"
#include "storage/note_storage.h"
#include "storage/session_lease.h"
#include "storage/session_database.h"
#include "storage/sqlite_storage.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "runtime/command_queue.h"
#include "workspace/builtins.h"
#include "workspace/workspace_config_store.h"
#include "util/base64.h"
#include "util/path_name.h"

#include <gtest/gtest.h>
#include <array>
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
        if (message.contains("ok") && message.contains("id") && message["id"] == id) return message;
    }
    return {};
}

nlohmann::json folder_store_wire_fixture() {
    std::ifstream file(std::string(CHA_WIRE_FIXTURE_DIRECTORY) + "/folder-store-contract.json");
    return nlohmann::json::parse(file);
}

nlohmann::json ordinary_note_items(const nlohmann::json& items) {
    auto ordinary = nlohmann::json::array();
    for (const auto& item : items) {
        if (!item.contains("forum_id")) ordinary.push_back(item);
    }
    return ordinary;
}

class BridgeRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        database_ = test::import_test_database(workspace_.root());
        RuntimeSettings settings;
        settings.test_hooks.note_compression_wait = [this](int seconds) { if (compression_wait_) compression_wait_(seconds); };
        application_ = app::Application::open(make_command(workspace_, database_), {}, settings);
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
        for (const auto deadline = std::chrono::steady_clock::now() + 5s;
             std::chrono::steady_clock::now() < deadline;) {
            const auto batch = wait_delivery(*router_, connection_);
            if (!batch) break;
            ack_delivery(*router_, connection_, *batch);
            auto reply = reply_with_id(*batch, id);
            if (!reply.empty()) return reply;
        }
        ADD_FAILURE() << "No reply for " << method;
        return {};
    }

    void wait_compression(std::int64_t id) {
        for (const auto deadline = std::chrono::steady_clock::now() + 5s;
             std::chrono::steady_clock::now() < deadline;) {
            if (!application_->note_compression_status(id, epoch_)["pending"].get<bool>()) return;
            std::this_thread::sleep_for(2ms);
        }
        ADD_FAILURE() << "Compression did not retire";
    }

    // Connection cleanup releases summary reservations in a background job.
    nlohmann::json generate_summary_after_cleanup(const nlohmann::json& identity) {
        for (const auto deadline = std::chrono::steady_clock::now() + 5s;;) {
            auto reply = call("sessionSummary.generate", identity);
            if (reply.value("ok", false) || std::chrono::steady_clock::now() >= deadline
                || reply["error"].value("message", "") != "A summary is already being saved for this session.") return reply;
            std::this_thread::sleep_for(2ms);
        }
    }

    std::size_t summary_events(std::string_view text) {
        const auto events = application_->list_events(epoch_);
        return static_cast<std::size_t>(std::count_if(events.begin(), events.end(),
            [text](const auto& event) { return event.message.find(text) != std::string::npos; }));
    }

    // Provider/result validation tests wait for Events; acceptance has its own tests.
    nlohmann::json compress_and_wait(nlohmann::json params) {
        auto accepted = call("notes.compressStart", params);
        if (!accepted.value("ok", false) || accepted["result"].contains("skipped")) return accepted;
        wait_compression(params.at("id").get<std::int64_t>());
        const auto events = application_->list_events(epoch_);
        if (!events.empty() && events.front().severity == EventSeverity::error) {
            auto message = events.front().message;
            const auto reason = message.find(": ");
            if (reason != std::string::npos) message.erase(0, reason + 2);
            return {{"ok", false}, {"error", {{"code", "internal_error"}, {"message", message}}}};
        }
        const bool committed = !events.empty() && events.front().severity == EventSeverity::success;
        EXPECT_TRUE(committed) << "Expected a committed compression success event";
        return {{"ok", committed}, {"result", nlohmann::json::object()}};
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::unique_ptr<app::Application> application_;
    std::function<void(int)> compression_wait_;
    std::unique_ptr<BridgeRouter> router_;
    std::string connection_;
    std::uint64_t epoch_{1};
    std::uint64_t next_id_{1};
};

TEST_F(BridgeRouterTest, PromptMethodsUseExactParametersAndVaultEpoch) {
    bootstrap_epoch();
    std::ifstream input(std::filesystem::path(CHA_WIRE_FIXTURE_DIRECTORY) / "prompt-contract.json");
    const auto fixture = nlohmann::json::parse(input);
    EXPECT_EQ(call("prompt.list")["result"], fixture["list"]);
    const auto original = call("prompt.get", fixture["get"])["result"];
    EXPECT_FALSE(original["text"].get<std::string>().empty());
    const nlohmann::json l1_request{{"prompt_id", "summary-l1"}};
    const auto l1 = call("prompt.get", l1_request)["result"];
    ASSERT_EQ(l1["id"], "summary-l1");
    EXPECT_FALSE(l1["text"].get<std::string>().empty());
    EXPECT_EQ(call("prompt.update", fixture["update"])["result"], fixture["detail"]);
    EXPECT_EQ(call("prompt.get", fixture["get"])["result"], fixture["detail"]);
    EXPECT_EQ(call("prompt.get", l1_request)["result"], l1);
    for (const auto method : {"prompt.get", "prompt.update"}) {
        nlohmann::json params{{"prompt_id", "unknown-prompt"}};
        if (std::string_view(method) == "prompt.update") params["text"] = "";
        const auto reply = call(method, params);
        EXPECT_EQ(reply["error"]["code"], "not_found");
        EXPECT_EQ(reply["error"]["message"], "That prompt was not found.");
    }
    EXPECT_EQ(call("prompt.list", {{"text", ""}})["error"]["code"], "invalid_argument");
    EXPECT_EQ(call("prompt.get", {{"prompt_id", "../a"}})["error"]["code"], "invalid_argument");
    EXPECT_EQ(call("prompt.update", fixture["get"])["error"]["code"], "invalid_argument");
    EXPECT_EQ(call("prompt.update", {{"prompt_id", "note-compression"}, {"text", nullptr}})["error"]["code"], "invalid_argument");
    auto extra = fixture["update"];
    extra["display_name"] = "Renamed";
    EXPECT_EQ(call("prompt.update", extra)["error"]["code"], "invalid_argument");
    extra = fixture["update"];
    extra["text"] = std::string(1, '\0');
    EXPECT_EQ(call("prompt.update", extra)["error"]["message"], "Choose a UTF-8 text file.");
    EXPECT_EQ(call("prompt.get", fixture["get"])["result"], fixture["detail"]);

    ++epoch_;
    for (const auto method : {"prompt.list", "prompt.get", "prompt.update"}) {
        const auto reply = call(method);
        EXPECT_EQ(reply["error"]["code"], "vault_changed");
    }
}

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
    EXPECT_EQ(call("notes.list")["result"]["items"].size(), 2u);
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
    EXPECT_EQ(call("notes.list")["result"]["items"].size(), 2u);
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
    EXPECT_EQ(reply["error"]["message"],
        "Page reading is not configured. Enable web_reader or Firecrawl in Settings → Search API.");

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
    EXPECT_EQ(call("character.picture.get", {{"character_id", "guide"}})["result"]["filename"], "Seneca.png");
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

TEST_F(BridgeRouterTest, NoteAudioListsVoicesFromTheConfiguredServiceAcrossPages) {
    bootstrap_epoch();
    EXPECT_EQ(call("noteAudio.listVoices")["error"]["code"], "invalid_argument");
    MockHttpServer server({
        http_response("application/json", R"({
            "voices":[{"id":"achernar","displayName":"Achernar","type":"prebuilt",
                "gender":"female","pitch":"high","languageCode":"en-US","regionCode":"US",
                "accent":"General American","persona":"Storyteller & Narrator","context":"Content & Media",
                "description":"Soft, calm voice suited to quiet storytelling."}],
            "nextPageToken":"page+/="
        })"),
        http_response("application/json", R"({
            "voices":[{"id":"voice_custom123","display_name":"Custom narrator","type":"replicated",
                "pitch":null,"description":" "}]
        })"),
    });
    const auto key = application_->create_api_key({.display_name = "Audio", .value = "audio-secret"}, epoch_);
    (void)application_->save_notes_settings({.audio_url = "http://127.0.0.1:" + std::to_string(server.port()),
        .audio_api_key = key.id}, epoch_);
    server.start();
    const auto reply = call("noteAudio.listVoices");
    server.join();
    ASSERT_TRUE(reply["ok"].get<bool>()) << reply;
    EXPECT_EQ(reply["result"], (nlohmann::json{{"voices", nlohmann::json::array({
        {{"id", "achernar"}, {"display_name", "Achernar"}, {"gender", "female"}, {"pitch", "high"},
            {"language_code", "en-US"}, {"region_code", "US"}, {"accent", "General American"},
            {"persona", "Storyteller & Narrator"}, {"context", "Content & Media"},
            {"description", "Soft, calm voice suited to quiet storytelling."}},
        {{"id", "voice_custom123"}, {"display_name", "Custom narrator"}},
    })}}));
    const auto& requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(requests[0].starts_with("GET /v1beta/voices?page_size=1000 "));
    EXPECT_TRUE(requests[1].starts_with("GET /v1beta/voices?page_size=1000&page_token=page%2B%2F%3D "));
    for (const auto& request : requests)
        EXPECT_NE(request.find("x-goog-api-key: audio-secret\r\n"), std::string::npos);
}

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
    EXPECT_EQ(call("notes.list")["result"]["items"].size(), 2u);
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
    const auto key = call("apiKey.create", {{"display_name", "OpenRouter"}, {"value", "test-secret"}, {"cloud_store", "none"}});
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
    const auto firecrawl = call("apiKey.create", {{"display_name", "Firecrawl"}, {"value", "fc-secret"}, {"cloud_store", "none"}});
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
    const auto firecrawl = call("apiKey.create", {{"display_name", "Firecrawl"}, {"value", "fc-secret"}, {"cloud_store", "none"}});
    ASSERT_TRUE(firecrawl["ok"]);
    settings["firecrawl_api_key"] = firecrawl["result"]["id"];
    settings["read_provider"] = "firecrawl";
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    EXPECT_EQ(call("apiKey.list")["result"][0]["used_by"], nlohmann::json::array({"Page reading"}));
    const auto reader = call("apiKey.create", {{"display_name", "web_reader"}, {"value", "reader-secret"}, {"cloud_store", "none"}});
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
        {"display_name", "Search"}, {"value", "test-secret"}, {"cloud_store", "none"}});
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
    reply = call("session.setCharacterSettings", {
        {"forum_id", "lobby"}, {"session_id", session_id},
        {"character_id", "guide"}, {"provider_id", "test"}, {"reasoning_effort", "high"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["clear_input"], false);
    reply = call("session.snapshot", {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["chat_settings"]["characters"][0]["reasoning_effort"], "high");
    reply = call("session.setCharacterSettings", {
        {"forum_id", "lobby"}, {"session_id", session_id},
        {"character_id", "guide"}, {"provider_id", "removed"}, {"reasoning_effort", "low"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_TRUE(reply["result"].contains("notice"));
    reply = call("session.snapshot", {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["chat_settings"]["characters"][0]["reasoning_effort"], "high");
    reply = call("session.setCharacterSettings", {
        {"forum_id", "lobby"}, {"session_id", session_id}, {"character_id", "guide"}});
    EXPECT_FALSE(reply["ok"]);
    reply = call("session.setCharacterSettings", {
        {"forum_id", "lobby"}, {"session_id", session_id}, {"character_id", "guide"},
        {"reasoning_effort", nullptr}, {"extra", "bad"}});
    EXPECT_FALSE(reply["ok"]);
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

TEST_F(BridgeRouterTest, SecondaryProcessCannotChangeForumLibrary) {
    bootstrap_epoch();
    const auto key = application_->create_api_key(
        {.display_name = "Library", .value = "test-secret", .cloud_store = "openai"}, epoch_);
    const ForumLibrary library{key.id, {"vs_original"}, std::nullopt};
    (void)application_->update_forum_library("lobby", library, epoch_);
    auto secondary = app::Application::open(make_command(workspace_, database_));
    BridgeRouter router(*secondary);
    const auto connection = router.open_connection();
    for (const auto& replacement : {nlohmann::json(nullptr), nlohmann::json{
            {"api_key_id", key.id}, {"store_ids", {"vs_replacement"}}, {"provider_id", nullptr}, {"catalogue", nlohmann::json::object()}}}) {
        const auto id = next_id_++;
        router.handle_request(connection, request_json(connection, id, secondary->context_epoch(),
            "forum.library.update", {{"forum_id", "lobby"}, {"library", replacement}}).dump());
        const auto batch = wait_delivery(router, connection);
        ASSERT_TRUE(batch);
        const auto reply = reply_with_id(*batch, id);
        ASSERT_FALSE(reply.empty());
        EXPECT_EQ(reply["ok"], false);
        EXPECT_EQ(reply["error"]["message"], "Settings can be changed only in the main CHA process.");
        ack_delivery(router, connection, *batch);
        const auto saved = application_->get_forum("lobby", epoch_).library;
        ASSERT_TRUE(saved);
        EXPECT_EQ(saved->api_key_id, library.api_key_id);
        EXPECT_EQ(saved->store_ids, library.store_ids);
    }
    router.shutdown();
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

    // Notes listing also uses the ordinary request limit.
    router_->handle_request(
        connection_, request_json(connection_, 2, 1, "notes.list").dump());
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

TEST_F(BridgeRouterTest, UploadTransferLetsOtherRequestsRunAndRefusesMaintenance) {
    bootstrap_epoch();
    MockHttpServer server({
        etag_response("uploader-etag"),
        etag_response("uploaded-etag"),
        etag_response("vault-etag"),
    });
    server.pause_before_response(2);
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket/",
        .access_key_id = "access",
        .secret_key = std::string("secret"),
    }, epoch_);
    server.start();
    const auto upload_id = next_id_++;
    router_->handle_request(connection_, request_json(
        connection_, upload_id, epoch_, "vault.upload", {{"etag", nullptr}}).dump());
    // The worker makes the copy; the paused database transfer runs in the background.
    router_->run_tasks();
    ASSERT_TRUE(server.wait_for_requests(2, 2s));
    epoch_ = application_->context_epoch();

    EXPECT_TRUE(call("app.bootstrap")["ok"]);
    EXPECT_TRUE(call("notes.list")["ok"]);
    const auto download = call("vault.download");
    ASSERT_FALSE(download["ok"]);
    EXPECT_EQ(download["error"]["code"], "invalid_argument");
    EXPECT_EQ(download["error"]["message"],
        "A vault upload is running. Try again when it finishes.");

    server.resume_responses();
    nlohmann::json uploaded;
    for (const auto deadline = std::chrono::steady_clock::now() + 5s;
         uploaded.empty() && std::chrono::steady_clock::now() < deadline;) {
        const auto batch = wait_delivery(*router_, connection_);
        if (!batch) continue;
        ack_delivery(*router_, connection_, *batch);
        uploaded = reply_with_id(*batch, upload_id);
    }
    server.join();
    ASSERT_FALSE(uploaded.empty());
    ASSERT_TRUE(uploaded["ok"]);
    EXPECT_GT(uploaded["result"]["byte_count"].get<std::uint64_t>(), 0U);
    EXPECT_EQ(uploaded["result"]["context_epoch"], epoch_);
    EXPECT_EQ(uploaded["context_epoch"], epoch_);
    EXPECT_EQ(application_->current_vault().get().r2_etag, "uploaded-etag");
    for (const auto& entry : std::filesystem::directory_iterator(
             application_->current_vault().get().data.parent_path())) {
        EXPECT_EQ(entry.path().filename().string().find(".upload."), std::string::npos);
    }
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

TEST_F(BridgeRouterTest, SavesApiKeyCloudStoreWithOrWithoutANewValue) {
    bootstrap_epoch();
    EXPECT_FALSE(call(
        "apiKey.create",
        {{"display_name", "OpenAI"}, {"value", "secret-one"}})["ok"]);
    auto reply = call(
        "apiKey.create",
        {{"display_name", "OpenAI"}, {"value", "secret-one"}, {"cloud_store", "openai"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"].at("cloud_store"), "openai");
    const std::string id = reply["result"].at("id");

    reply = call("apiKey.replaceValue", {{"api_key_id", id}, {"cloud_store", "none"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"].at("cloud_store"), "none");
    EXPECT_EQ(application_->api_keys().value(id), "secret-one");

    reply = call(
        "apiKey.replaceValue",
        {{"api_key_id", id}, {"value", "secret-two"}, {"cloud_store", "google"}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"].at("cloud_store"), "google");
    EXPECT_EQ(application_->api_keys().value(id), "secret-two");
    EXPECT_FALSE(call(
        "apiKey.replaceValue", {{"api_key_id", id}, {"value", "secret-three"}})["ok"]);
    EXPECT_FALSE(call(
        "apiKey.replaceValue", {{"api_key_id", id}, {"cloud_store", "anthropic"}})["ok"]);
    EXPECT_EQ(application_->api_keys().find(id)->cloud_store, "google");
}

TEST_F(BridgeRouterTest, CloudStoreMethodsUseTheSelectedKeyAndItsService) {
    bootstrap_epoch();
    const auto created = call("apiKey.create",
        {{"display_name", "OpenAI"}, {"value", "sk-cloud"}, {"cloud_store", "openai"}});
    ASSERT_TRUE(created["ok"]);
    const std::string key = created["result"]["id"];
    std::mutex mutex;
    std::vector<std::string> requests;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        const std::lock_guard lock(mutex);
        requests.push_back(request.method + " " + request.url);
        EXPECT_EQ(request.headers.front().value, "Bearer sk-cloud");
        if (request.url.ends_with("/vector_stores?limit=100"))
            return FolderStoreHttpResponse{.status = 200,
                .body = R"({"data":[{"id":"vs_1","name":"Research"},{"id":"vs_2","name":null}],"has_more":false})"};
        if (request.method == "GET" && request.url.ends_with("/vector_stores/vs_1"))
            return FolderStoreHttpResponse{.status = 200,
                .body = R"({"id":"vs_1","name":"Research","description":"Notes","created_at":1793697300})"};
        if (request.method == "GET" && request.url.ends_with("/vector_stores/vs_2"))
            return FolderStoreHttpResponse{.status = 200,
                .body = R"({"id":"vs_2","name":null,"created_at":1793697300})"};
        if (request.method == "POST" && request.url.ends_with("/vector_stores/vs_1")) {
            EXPECT_EQ(nlohmann::json::parse(request.body),
                (nlohmann::json{{"name", "Research"}, {"metadata", {{"description", "Notes"}}}}));
            return FolderStoreHttpResponse{.status = 200, .body = R"({"id":"vs_1"})"};
        }
        if (request.url.ends_with("/files?limit=100"))
            return FolderStoreHttpResponse{.status = 200, .body = R"({"data":[],"has_more":false})"};
        return FolderStoreHttpResponse{.status = 200, .body = R"({"deleted":true})"};
    });

    auto reply = call("cloudStore.list", {{"api_key_id", key}});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    EXPECT_EQ(reply["result"], nlohmann::json::parse(
        R"({"items":[{"id":"vs_1","name":"Research"},{"id":"vs_2","name":""}]})"));
    reply = call("cloudStore.get", {{"api_key_id", key}, {"store_id", "vs_1"}});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    EXPECT_EQ(reply["result"], nlohmann::json::parse(
        R"({"id":"vs_1","name":"Research","description":"Notes","created_at":"2026-11-03T09:15:00Z"})"));
    const auto before_refresh = [&] { const std::lock_guard lock(mutex); return requests.size(); }();
    reply = call("cloudStore.list", {{"api_key_id", key}, {"refresh", true}});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    EXPECT_EQ([&] { const std::lock_guard lock(mutex); return requests.size(); }(), before_refresh + 3);
    EXPECT_FALSE(call("cloudStore.list", {{"api_key_id", key}, {"refresh", "true"}})["ok"]);
    reply = call("cloudStore.update", {{"api_key_id", key}, {"store_id", "vs_1"}, {"name", "Research"}, {"description", "Notes"}});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    EXPECT_EQ(reply["result"]["name"], "Research");
    EXPECT_EQ(reply["result"]["description"], "Notes");
    EXPECT_FALSE(call("cloudStore.update", {{"api_key_id", key}, {"store_id", "vs_1"}, {"name", "Research"}})["ok"]);
    {
        const auto sent = [&] { const std::lock_guard lock(mutex); return requests.size(); }();
        reply = call("cloudStore.update", {{"api_key_id", key}, {"store_id", "vs_1"}, {"name", "Research"},
            {"description", std::string(513, 'x')}});
        EXPECT_EQ(reply["error"]["code"], "invalid_argument");
        EXPECT_EQ(reply["error"]["message"], "Description must be 512 characters or fewer for OpenAI.");
        const std::lock_guard lock(mutex);
        EXPECT_EQ(requests.size(), sent);
    }
    reply = call("cloudStore.delete", {{"api_key_id", key}, {"store_id", "vs_1"}});
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    EXPECT_EQ(reply["result"], nlohmann::json::object());
    {
        const std::lock_guard lock(mutex);
        EXPECT_EQ(requests.back(), "DELETE https://api.openai.com/v1/vector_stores/vs_1");
    }

    EXPECT_EQ(call("cloudStore.get", {{"api_key_id", key}, {"store_id", "../files"}})["error"]["code"], "invalid_argument");
    EXPECT_EQ(call("cloudStore.list", {{"api_key_id", "api_key_999"}})["error"]["code"], "not_found");
    EXPECT_FALSE(call("cloudStore.list", {{"api_key_id", key}, {"extra", 1}})["ok"]);
    ASSERT_TRUE(call("apiKey.replaceValue", {{"api_key_id", key}, {"cloud_store", "none"}})["ok"]);
    const auto before = [&] { const std::lock_guard lock(mutex); return requests.size(); }();
    reply = call("cloudStore.list", {{"api_key_id", key}});
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    EXPECT_EQ(reply["error"]["message"], "Set cloud store API key");
    const std::lock_guard lock(mutex);
    EXPECT_EQ(requests.size(), before);
}

TEST_F(BridgeRouterTest, CloudStoreListingCanFinishItsMetadataReadsPastTheCommandDeadline) {
    router_ = std::make_unique<BridgeRouter>(*application_,
        BridgeRouter::Options{.platform = "test", .command_deadline = 0ms});
    connection_ = router_->open_connection();
    bootstrap_epoch();
    const auto key = application_->create_api_key(
        {.display_name = "Gemini", .value = "test-key", .cloud_store = "google"}, epoch_).id;
    application_->set_folder_store_transport_for_tests([](const auto& request, const auto&) {
        if (request.url.find("/documents?") != std::string::npos)
            return FolderStoreHttpResponse{.status = 200, .body = "{}"};
        if (request.url.ends_with("/fileSearchStores/a"))
            return FolderStoreHttpResponse{.status = 200,
                .body = R"({"name":"fileSearchStores/a","displayName":"Research","createTime":"2026-11-03T09:15:00Z"})"};
        return FolderStoreHttpResponse{.status = 200,
            .body = R"({"fileSearchStores":[{"name":"fileSearchStores/a","displayName":"Research"}]})"};
    });
    const auto id = next_id_++;
    router_->handle_request(connection_, request_json(connection_, id, epoch_, "cloudStore.list", {{"api_key_id", key}}).dump());
    router_->expire_timeouts();
    const auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    const auto reply = reply_with_id(*batch, id);
    ASSERT_TRUE(reply["ok"]) << reply.dump();
    EXPECT_EQ(reply["result"]["items"][0]["name"], "Research");
    ack_delivery(*router_, connection_, *batch);
}

TEST_F(BridgeRouterTest, CloudStoreCancelStopsDeletionBeforeTheNextProviderCall) {
    bootstrap_epoch();
    const auto created = call("apiKey.create",
        {{"display_name", "OpenAI"}, {"value", "sk-cloud"}, {"cloud_store", "openai"}});
    ASSERT_TRUE(created["ok"]);
    const std::string key = created["result"]["id"];
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, released = false, cancelled = false;
    std::vector<std::string> requests;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        std::unique_lock lock(mutex);
        if (cancelled) ADD_FAILURE() << "Provider call after Cancel: " << request.url;
        requests.push_back(request.method + " " + request.url);
        if (request.method == "GET")
            return FolderStoreHttpResponse{.status = 200,
                .body = R"({"data":[{"id":"file_1"},{"id":"file_2"}],"has_more":false})"};
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return released; });
        return FolderStoreHttpResponse{.status = 200, .body = R"({"deleted":true})"};
    });
    const auto delete_id = next_id_++;
    router_->handle_request(connection_, request_json(connection_, delete_id, epoch_, "cloudStore.delete",
        {{"api_key_id", key}, {"store_id", "vs_1"}}).dump());
    router_->run_tasks();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 5s, [&] { return entered; }));
    }
    const auto cancel_id = next_id_++;
    router_->handle_request(connection_, request_json(connection_, cancel_id, epoch_, "cloudStore.cancel",
        {{"request_id", delete_id}}).dump());
    std::map<std::uint64_t, nlohmann::json> replies;
    for (const auto deadline = std::chrono::steady_clock::now() + 5s;
         replies.size() < 2 && std::chrono::steady_clock::now() < deadline;) {
        const auto batch = wait_delivery(*router_, connection_);
        if (!batch) break;
        ack_delivery(*router_, connection_, *batch);
        for (const auto id : {delete_id, cancel_id}) {
            auto reply = reply_with_id(*batch, id);
            if (!reply.empty()) replies[id] = std::move(reply);
        }
    }
    ASSERT_EQ(replies.size(), 2u);
    EXPECT_TRUE(replies[cancel_id]["ok"]);
    EXPECT_EQ(replies[delete_id]["error"]["code"], "operation_cancelled");
    {
        const std::lock_guard lock(mutex);
        cancelled = true;
        released = true;
    }
    changed.notify_all();
    // The worker finishes its current call and starts no other provider call.
    std::this_thread::sleep_for(100ms);
    const std::lock_guard lock(mutex);
    EXPECT_EQ(requests, (std::vector<std::string>{
        "GET https://api.openai.com/v1/vector_stores/vs_1/files?limit=100",
        "DELETE https://api.openai.com/v1/files/file_1"}));
}

TEST_F(BridgeRouterTest, ListsProvidersAndApiKeyMetadataWithoutSecrets) {
    bootstrap_epoch();
    auto reply = call("provider.list");
    ASSERT_TRUE(reply["ok"]);
    ASSERT_FALSE(reply["result"].empty());
    EXPECT_EQ(reply["result"].front().at("id"), "test");
    reply = call("provider.list", {{"api_key_id", "missing"}});
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");

    reply = call(
        "apiKey.create",
        {{"display_name", "Router"}, {"value", "private-router-secret"}, {"cloud_store", "none"}});
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
        {"display_name", "Fish"}, {"value", "fish-secret"}, {"cloud_store", "none"},
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
    auto statement = database.prepare("SELECT content_json FROM note_items WHERE parent_id=? AND compressed=1", id);
    if (!statement.step()) return {};
    return note_document_markdown(note_parse_saved_document(statement.blob(0), id));
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

TEST_F(BridgeRouterTest, NotePageProgressReportsFormattingOnlyToItsRequestingConnection) {
    bootstrap_epoch();
    for (const bool youtube : {false, true}) {
        SCOPED_TRACE(youtube);
        const auto id = youtube ? "youtube-transcript" : "page-formatting";
        const auto prompt = youtube ? "Custom transcript instructions" : "Custom article instructions";
        (void)application_->update_prompt(id, {prompt}, epoch_);
        const std::string url = youtube ? "https://youtu.be/video" : "https://example.org/article";
        const nlohmann::json page{{"url", url}, {"title", "Title"},
            {"markdown", youtube ? "Raw transcript" : std::string(5001, 'a')}};
        MockHttpServer reader({http_response("application/json", page.dump())});
        MockHttpServer provider({compression_response("chat_completions", false, "Formatted content")});
        reader.pause_before_response(1);
        provider.pause_before_response(1);
        reader.start();
        provider.start();
        configure_compression(*application_, provider.port(), "chat_completions", false, epoch_);
        auto settings = call("webSearch.get")["result"];
        settings["web_reader_enabled"] = true;
        settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
        ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
        const auto other = router_->open_connection();
        const auto read = queue_request(*router_, connection_, next_id_, epoch_, "notes.readPage", {{"url", url}});
        router_->run_ordinary_tasks();
        ASSERT_TRUE(reader.wait_for_requests(1, 2s));
        EXPECT_FALSE(router_->take_delivery(connection_));
        (void)application_->update_prompt(id, {"Saved during download"}, epoch_);
        reader.resume_responses();
        ASSERT_TRUE(provider.wait_for_requests(1, 2s));
        EXPECT_TRUE(router_->wait_for_work(10ms));
        auto batch = wait_delivery(*router_, connection_);
        ASSERT_TRUE(batch);
        ASSERT_EQ(batch->at("messages").size(), 1U) << *batch;
        EXPECT_EQ(batch->at("messages").front(), (nlohmann::json{
            {"connection_id", connection_}, {"context_epoch", epoch_},
            {"event", "notes.pageProgress"}, {"request_id", read},
            {"stage", youtube ? "formatting_transcript" : "formatting_article"}}));
        EXPECT_FALSE(router_->take_delivery(other));
        ack_delivery(*router_, connection_, *batch);
        EXPECT_FALSE(router_->take_delivery(connection_));
        provider.resume_responses();
        const auto replies = wait_replies(*router_, connection_, {read});
        ASSERT_EQ(replies.size(), 1U);
        EXPECT_TRUE(replies.at(read)["ok"]) << replies.at(read);
        reader.join();
        provider.join();
        ASSERT_EQ(provider.requests().size(), 1U);
        const auto body = nlohmann::json::parse(request_body(provider.requests().front()));
        EXPECT_EQ(body["messages"][0]["content"], prompt);
        EXPECT_FALSE(router_->take_delivery(connection_));
        router_->close_connection(other);
    }
}

TEST_F(BridgeRouterTest, NotePageProgressFromAReplacedVaultIsNotDelivered) {
    bootstrap_epoch();
    const nlohmann::json page{{"url", "https://youtu.be/video"}, {"title", "Video"}, {"markdown", "Transcript"}};
    MockHttpServer reader({http_response("application/json", page.dump())});
    MockHttpServer provider({compression_response("chat_completions", false, "Formatted content")});
    provider.pause_before_response(1);
    reader.start();
    provider.start();
    configure_compression(*application_, provider.port(), "chat_completions", false, epoch_);
    auto settings = call("webSearch.get")["result"];
    settings["web_reader_enabled"] = true;
    settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
    ASSERT_TRUE(call("webSearch.save", settings)["ok"]);
    const auto copy = application_->create_vault(VaultCreate{
        .display_name = "Copied", .copy_from = "Test", .password = {},
    }, epoch_);
    const auto read = queue_request(*router_, connection_, next_id_, epoch_, "notes.readPage", {{"url", page["url"]}});
    router_->run_ordinary_tasks();
    ASSERT_TRUE(provider.wait_for_requests(1, 2s));
    // Leave the stage pending in the old request until after vault replacement.
    (void)application_->switch_vault(copy.name, {}, epoch_);
    auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    for (const auto& message : batch->at("messages")) EXPECT_NE(message.value("event", ""), "notes.pageProgress");
    ack_delivery(*router_, connection_, *batch);
    provider.resume_responses();
    const auto replies = wait_replies(*router_, connection_, {read});
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies.at(read)["error"]["code"], "vault_changed");
    reader.join();
    provider.join();
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
    EXPECT_EQ(call("notes.list")["result"]["items"].size(), 2u);
}

TEST_F(BridgeRouterTest, SafeLinksYoutubeNoteImportsUseTranscriptFormatting) {
    bootstrap_epoch();
    std::ifstream prompt_file(std::filesystem::path(CHA_SOURCE_DIRECTORY) / "resources/youtube-transcript.md");
    const std::string prompt{std::istreambuf_iterator<char>(prompt_file), std::istreambuf_iterator<char>()};
    ASSERT_FALSE(prompt.empty());
    const std::string regional = "https://na01.safelinks.protection.outlook.com/"
        "?url=https%3A%2F%2Fwww.youtube.com%2Flive%2Fvideo%3Fsi%3Dtracking&data=ignored&reserved=0";
    const std::string short_link = "https://SAFELINKS.PROTECTION.OUTLOOK.COM./"
        "?data=ignored&url=https%3A%2F%2Fyoutu.be%2Fvideo%3Ft%3D12&reserved=0";
    for (const auto& [url, final_url] : std::vector<std::pair<std::string, std::string>>{
            {regional, regional}, {short_link, short_link}, {"https://example.org/redirect", regional}}) {
        SCOPED_TRACE(url);
        const nlohmann::json page{{"url", final_url}, {"title", "Video"},
            {"markdown", "00:00 A short transcript."}};
        MockHttpServer reader({http_response("application/json", page.dump())});
        MockHttpServer provider({compression_response("chat_completions", false, "Formatted transcript.")});
        reader.start();
        provider.start();
        configure_compression(*application_, provider.port(), "chat_completions", false, epoch_);
        auto settings = call("webSearch.get")["result"];
        settings["web_reader_enabled"] = true;
        settings["web_reader_url"] = "http://127.0.0.1:" + std::to_string(reader.port());
        ASSERT_TRUE(call("webSearch.save", settings)["ok"]);

        const auto reply = call("notes.readPage", {{"url", url}});
        reader.join();
        provider.join();
        ASSERT_TRUE(reply["ok"]) << reply;
        EXPECT_EQ(reply["result"], (nlohmann::json{{"url", final_url}, {"title", "Video"},
            {"markdown", "# Video\n\nFormatted transcript."}}));
        ASSERT_EQ(provider.requests().size(), 1U);
        const auto body = nlohmann::json::parse(request_body(provider.requests().front()));
        EXPECT_EQ(body["messages"][0]["content"], prompt);
        EXPECT_EQ(body["messages"][1]["content"], "from User:\n# Video\n\n00:00 A short transcript.");
        // Unwrapping affects formatter selection, not the URL sent to the reader.
        EXPECT_EQ(nlohmann::json::parse(request_body(reader.requests().front()))["url"], url);
    }
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
            "https://notyoutube.com/watch", "https://example.org/?url=https://youtu.be/video",
            "https://na01.safelinks.protection.outlook.com/?url=https%3A%2F%2Fexample.org%2Fyoutube.com",
            "https://safelinks.protection.outlook.com.evil.example/?url=https%3A%2F%2Fyoutu.be%2Fvideo",
            "https://notsafelinks.protection.outlook.com/?url=https%3A%2F%2Fyoutu.be%2Fvideo",
            "https://na01.safelinks.protection.outlook.com/?data=ignored",
            "https://na01.safelinks.protection.outlook.com/?url=",
            "https://na01.safelinks.protection.outlook.com/?url=not-a-url",
            "https://na01.safelinks.protection.outlook.com/?url=https%3A%2F%2Fyoutu.be%2Fvideo%00ignored"}) {
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
        "Select a compression provider in Notes settings.");
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
            expect_note_error(compress_and_wait({{"id", note.id}}), "internal_error",
                "The provider refused to compress this note.");
            EXPECT_FALSE(application_->list_note_items(epoch_).back().has_compressed);
            ASSERT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
            for (int refusal = 0; refusal < 2; ++refusal) {
                expect_note_error(compress_and_wait({{"id", note.id}}), "internal_error",
                    "The provider refused to compress this note.");
                EXPECT_EQ(stored_compression(database_, note.id), "Saved compression");
                EXPECT_TRUE(application_->list_note_items(epoch_).back().has_compressed);
            }
            EXPECT_EQ(application_->get_note(note.id, epoch_).document, original);
            EXPECT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
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
            EXPECT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
            EXPECT_EQ(stored_compression(database_, note.id), "First compression");
            EXPECT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
            EXPECT_EQ(stored_compression(database_, note.id), "Updated compression");
            EXPECT_FALSE(compress_and_wait({{"id", note.id}})["ok"]);
            EXPECT_FALSE(compress_and_wait({{"id", note.id}})["ok"]);
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
            ASSERT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
            server.join();
            ASSERT_EQ(server.requests().size(), 1U);
            const auto body = nlohmann::json::parse(request_body(server.requests().front()));
            if (std::string_view(api) == "responses") EXPECT_EQ(body["reasoning"]["effort"], effort);
            else EXPECT_EQ(body["reasoning_effort"], effort);
            EXPECT_EQ(application_->get_provider("test", epoch_).reasoning_effort, "low");
        }
    }
}

FullSessionId summary_source(app::Application& application, const std::filesystem::path& path, std::uint64_t epoch) {
    const auto session = application.create_session("lobby", "Summary source", epoch);
    storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
    auto key = database.prepare("SELECT session_key FROM sessions WHERE session_id=?", session.id);
    if (!key.step()) throw std::runtime_error("Missing test session");
    SessionJournal journal(path, key.integer(0));
    journal.record_entry(make_human_entry({.id=1, .author={"human", "User"}, .addressed_to={"-", "-"}, .text="Full saved goal"}));
    journal.record_entry(make_character_entry(2, "guide", "Guide", "Completed answer", EntryStatus::complete));
    journal.record_entry(make_character_entry(3, "guide", "Guide", "Cancelled answer", EntryStatus::cancelled));
    journal.record_entry(make_notice_entry(5, "Hidden notice"));
    journal.record_entry(make_error_entry(6, "Hidden error"));
    journal.record_entry(make_human_entry({.id=7, .author={"human", "User"}, .addressed_to={"-", "-"}, .text="Unanswered goal"}));
    return {"lobby", session.id};
}

nlohmann::json summary_identity(const FullSessionId& identity) {
    return {{"forum_id", identity.forum_id}, {"session_id", identity.session_id}};
}

nlohmann::json summary_document(std::string text) {
    auto document = text_document("Overview. Keywords: topic");
    const auto heading = [](std::string title) {
        return nlohmann::json{{"type", "heading"}, {"attrs", {{"level", 1}}},
            {"content", {{{"type", "text"}, {"text", std::move(title)}}}}};
    };
    document["content"].insert(document["content"].begin(), heading("Short summary"));
    document["content"].push_back(heading("Long summary"));
    document["content"].push_back(text_document(std::move(text))["content"][0]);
    return document;
}

TEST_F(BridgeRouterTest, SummaryUsesSavedSourceAndNotesProviderAndReplacesOneReadOnlyNote) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    (void)application_->update_prompt("session-short-summary", {"Short instructions"}, epoch_);
    (void)application_->update_prompt("session-long-summary", {"Long instructions"}, epoch_);
    for (const auto api : {"chat_completions", "responses"}) {
        MockHttpServer server({compression_response(api, false, "Overview. **Keywords:** topic"),
            compression_response(api, false, "Useful summary"),
            compression_response(api, false, "New overview. **Keywords:** topic"),
            compression_response(api, false, "## Details\n\nReplacement summary")});
        server.start();
        configure_compression(*application_, server.port(), api, false, epoch_);
        auto generated = call("sessionSummary.generate", summary_identity(source));
        ASSERT_TRUE(generated["ok"]) << generated;
        EXPECT_EQ(generated["result"]["markdown"],
            "# Short summary\n\nOverview. **Keywords:** topic\n\n# Long summary\n\nUseful summary");
        ASSERT_TRUE(call("sessionSummary.commit", {{"run_id", generated["result"]["run_id"]},
            {"document", summary_document("Useful summary")}})["ok"]);
        ASSERT_TRUE(call("session.open", summary_identity(source))["ok"]);
        auto snapshot = call("session.snapshot", summary_identity(source));
        ASSERT_TRUE(snapshot["ok"]) << snapshot;
        EXPECT_EQ(snapshot["result"]["transcript"].back()["kind"], "notice");
        const auto items = application_->list_note_items(epoch_);
        const auto saved = *std::find_if(items.begin(), items.end(), [](const auto& item) { return item.is_summary; });
        EXPECT_EQ(snapshot["result"]["transcript"].back()["text"], "“" + saved.name + "” is stored in Notes.");
        EXPECT_TRUE(saved.read_only);
        EXPECT_TRUE(saved.summary_linked);
        (void)application_->set_note_read_only(saved.id, false, epoch_);
        application_->save_note(saved.id, summary_document("User edits must not enter the prompt"), epoch_);
        generated = call("sessionSummary.generate", summary_identity(source));
        ASSERT_TRUE(generated["ok"]) << generated;
        EXPECT_EQ(generated["result"]["target_note_id"], saved.id);
        EXPECT_EQ(generated["result"]["markdown"],
            "# Short summary\n\nNew overview. **Keywords:** topic\n\n# Long summary\n\n## Details\n\nReplacement summary");
        // Replacing a summary keeps its name even if the session was renamed.
        ASSERT_TRUE(call("session.rename", {{"forum_id", source.forum_id}, {"session_id", source.session_id}, {"label", "Latest title"}})["ok"]);
        const auto committed = call("sessionSummary.commit", {{"run_id", generated["result"]["run_id"]},
            {"document", summary_document("Replacement summary")}});
        ASSERT_TRUE(committed["ok"]) << committed;
        EXPECT_EQ(committed["result"]["item"]["id"], saved.id);
        EXPECT_EQ(committed["result"]["item"]["name"], saved.name);
        snapshot = call("session.snapshot", summary_identity(source));
        ASSERT_TRUE(snapshot["ok"]) << snapshot;
        EXPECT_EQ(snapshot["result"]["transcript"].back()["text"], "“" + saved.name + "” is stored in Notes.");
        EXPECT_EQ(committed["result"]["item"]["read_only"], true);
        EXPECT_EQ(application_->get_note(saved.id, epoch_).document, summary_document("Replacement summary"));
        const auto events = application_->list_events(epoch_);
        EXPECT_NE(events.front().message.find("Summary saved"), std::string::npos);
        EXPECT_EQ(events.front().severity, EventSeverity::success);
        server.join();
        ASSERT_EQ(server.requests().size(), 4U);
        std::string transcript;
        for (std::size_t i = 0; i < server.requests().size(); ++i) {
            const auto body = nlohmann::json::parse(request_body(server.requests()[i]));
            const auto text = body.dump();
            EXPECT_FALSE(body.contains("tools"));
            EXPECT_NE(text.find("Full saved goal"), std::string::npos);
            EXPECT_NE(text.find("Completed answer"), std::string::npos);
            EXPECT_NE(text.find("Unanswered goal"), std::string::npos);
            const auto expected_prompt = i % 2 == 0 ? "Short instructions" : "Long instructions";
            const bool responses = std::string_view(api) == "responses";
            EXPECT_EQ(responses ? body["instructions"] : body["messages"][0]["content"], expected_prompt);
            const auto input = responses ? body["input"].dump() : body["messages"][1].dump();
            if (i % 2 == 0) transcript = input;
            else EXPECT_EQ(input, transcript);
            EXPECT_EQ(text.find("Overview."), std::string::npos);
            for (const auto excluded : {"Cancelled answer", "Failed answer", "Hidden notice", "Hidden error", "User edits must not", "is stored in Notes"})
                EXPECT_EQ(text.find(excluded), std::string::npos);
        }
    }
}

TEST_F(BridgeRouterTest, SummaryRetainsCapturedLongPromptAndRetriesOnlyTheLongRequest) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    const auto success = [](std::string text) { return compression_response("chat_completions", false, text); };
    MockHttpServer server({success("First short"),
        "HTTP/1.1 429 Limited\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        success("First long"), success("Next short"), success("Next long")});
    server.pause_before_response(1);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    (void)application_->update_prompt("session-short-summary", {"Short"}, epoch_);
    (void)application_->update_prompt("session-long-summary", {"Before edit"}, epoch_);
    const auto run = queue_request(*router_, connection_, next_id_, epoch_, "sessionSummary.generate", summary_identity(source));
    router_->run_ordinary_tasks();
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    (void)application_->update_prompt("session-long-summary", {"After edit"}, epoch_);
    server.resume_responses();
    const auto first = wait_replies(*router_, connection_, {run}).at(run);
    ASSERT_TRUE(first["ok"]) << first;
    ASSERT_TRUE(call("sessionSummary.abandon", {{"run_id", first["result"]["run_id"]},
        {"reason", "conversion_failed"}})["ok"]);
    const auto second = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(second["ok"]) << second;
    EXPECT_EQ(second["result"]["markdown"], "# Short summary\n\nNext short\n\n# Long summary\n\nNext long");
    server.join();
    const std::array expected{"Short", "Before edit", "Before edit", "Short", "After edit"};
    ASSERT_EQ(server.requests().size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto request = nlohmann::json::parse(request_body(server.requests()[i]));
        EXPECT_EQ(request["messages"][0]["content"], expected[i]);
    }
}

TEST_F(BridgeRouterTest, SummaryLongFailureLeavesTheExistingNoteIntact) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    const auto success = [](std::string text) { return compression_response("chat_completions", false, text); };
    MockHttpServer server({success("Short"), success("Long"), success("New short"),
        compression_refusal("chat_completions", false, false)});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto first = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(first["ok"]) << first;
    const auto document = summary_document("Existing long body");
    const auto committed = call("sessionSummary.commit", {{"run_id", first["result"]["run_id"]}, {"document", document}});
    ASSERT_TRUE(committed["ok"]) << committed;
    const auto failed = call("sessionSummary.generate", summary_identity(source));
    EXPECT_EQ(failed["error"]["code"], "internal_error");
    EXPECT_EQ(application_->get_note(committed["result"]["item"]["id"], epoch_).document, document);
    server.join();
}

TEST_F(BridgeRouterTest, SummaryReservationSurvivesDeliveryAndChecksOwnerAndAbandonBarrier) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    MockHttpServer server({compression_response("chat_completions", false, "Short"),
        compression_response("chat_completions", false, "Summary"),
        compression_response("chat_completions", false, "Next short"),
        compression_response("chat_completions", false, "Next summary")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    auto generated = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(generated["ok"]) << generated;
    const auto id = generated["result"]["run_id"].get<std::int64_t>();
    expect_note_error(call("sessionSummary.generate", summary_identity(source)), "invalid_argument",
        "A summary is already being saved for this session.");
    EXPECT_THROW((void)application_->commit_session_summary("other-window", id, summary_document("Wrong owner"), epoch_), app::ApplicationError);
    EXPECT_THROW(application_->abandon_session_summary("other-window", id, "conversion_failed", epoch_), app::ApplicationError);
    // A queued commit cannot write after abandon acknowledges removal of the run.
    const auto queued = queue_request(*router_, connection_, next_id_, epoch_, "sessionSummary.commit",
        {{"run_id", id}, {"document", summary_document("Too late")}});
    application_->abandon_session_summary(connection_, id, "commit_unknown", epoch_);
    router_->run_tasks();
    auto batch = wait_delivery(*router_, connection_);
    ASSERT_TRUE(batch);
    EXPECT_FALSE(reply_with_id(*batch, queued)["ok"]);
    ack_delivery(*router_, connection_, *batch);
    ASSERT_TRUE(call("sessionSummary.abandon", {{"run_id", id}, {"reason", "commit_unknown"}})["ok"]);
    generated = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(generated["ok"]) << generated;
    EXPECT_GT(generated["result"]["run_id"].get<std::int64_t>(), id);
    const auto second = generated["result"]["run_id"];
    ASSERT_TRUE(call("sessionSummary.abandon", {{"run_id", second}, {"reason", "conversion_failed"}})["ok"]);
    EXPECT_FALSE(call("sessionSummary.commit", {{"run_id", second}, {"document", summary_document("Discarded")}})["ok"]);
    EXPECT_EQ(application_->list_note_items(epoch_).size(), 2U);
    server.join();
}

TEST_F(BridgeRouterTest, SummaryRejectsIncompleteRefusedAndEmptyAnswersWithoutCreatingNotes) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    MockHttpServer server({compression_response("chat_completions", false, "Partial", true),
        compression_refusal("chat_completions", false, false), compression_response("chat_completions", false, " \n "),
        compression_response("chat_completions", false, "Short"),
        compression_response("chat_completions", false, "Good summary")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    for (int failure = 0; failure < 3; ++failure) {
        const auto result = call("sessionSummary.generate", summary_identity(source));
        EXPECT_FALSE(result["ok"]) << result;
        EXPECT_EQ(application_->list_note_items(epoch_).size(), 2U);
    }
    EXPECT_EQ(application_->list_events(epoch_).front().message, "Summary save failed for “Summary source” ("
        + source.forum_id + "/" + source.session_id + "): The provider returned no summary text.");
    const auto good = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(good["ok"]) << good;
    EXPECT_FALSE(call("sessionSummary.commit", {{"run_id", good["result"]["run_id"]}, {"document", nlohmann::json::object()}})["ok"]);
    EXPECT_EQ(application_->list_note_items(epoch_).size(), 2U);
    ASSERT_TRUE(call("session.open", summary_identity(source))["ok"]);
    const auto snapshot = call("session.snapshot", summary_identity(source));
    ASSERT_TRUE(snapshot["ok"]) << snapshot;
    for (const auto& entry : snapshot["result"]["transcript"])
        EXPECT_EQ(entry["text"].get<std::string>().find("is stored in Notes"), std::string::npos);
    server.join();
}

TEST_F(BridgeRouterTest, SummaryRejectsCombinedBodiesLargerThanTheNoteLimit) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    const std::string body(note_document_byte_limit / 2, 'x');
    const auto response = compression_response("chat_completions", false, body);
    MockHttpServer server({response, response});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    expect_note_error(call("sessionSummary.generate", summary_identity(source)), "body_too_large",
        "The generated summary exceeds 12 MiB.");
    EXPECT_EQ(application_->list_note_items(epoch_).size(), 2U);
    server.join();
}

TEST_F(BridgeRouterTest, SummaryReportsContextWindowWithNotesSettings) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    MockHttpServer server({"HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    expect_note_error(call("sessionSummary.generate", summary_identity(source)), "invalid_argument",
        "The conversation exceeds the model's context window. Select a model with a larger context window "
        "in Settings → Notes settings and try again.");
    server.join();
    EXPECT_EQ(application_->list_note_items(epoch_).size(), 2U);
}

TEST_F(BridgeRouterTest, SummaryDisconnectAfterMarkdownDeliveryReleasesReservation) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    MockHttpServer server({compression_response("chat_completions", false, "Short"),
        compression_response("chat_completions", false, "Summary"),
        compression_response("chat_completions", false, "Next short"),
        compression_response("chat_completions", false, "Next summary")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto first = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(first["ok"]) << first;
    router_->close_connection(connection_);
    connection_ = router_->open_connection();
    const auto next = generate_summary_after_cleanup(summary_identity(source));
    ASSERT_TRUE(next["ok"]) << next;
    EXPECT_NE(first["result"]["run_id"], next["result"]["run_id"]);
    EXPECT_FALSE(call("sessionSummary.commit", {{"run_id", first["result"]["run_id"]}, {"document", summary_document("Stale")}})["ok"]);
    EXPECT_TRUE(call("sessionSummary.commit", {{"run_id", next["result"]["run_id"]}, {"document", summary_document("Current")}})["ok"]);
    server.join();
}

TEST_F(BridgeRouterTest, SummaryDeletionDuringConversionAndCompressionAdmissionKeepIdentity) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    MockHttpServer server({compression_response("chat_completions", false, "Short"),
        compression_response("chat_completions", false, "Summary"),
        compression_response("chat_completions", false, "Next short"),
        compression_response("chat_completions", false, "Replacement")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    auto generated = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(generated["ok"]) << generated;
    auto saved = call("sessionSummary.commit", {{"run_id", generated["result"]["run_id"]}, {"document", summary_document("Summary")}});
    ASSERT_TRUE(saved["ok"]) << saved;
    const auto id = saved["result"]["item"]["id"];
    EXPECT_EQ(call("notes.compressStart", {{"id", id}, {"skip_ineligible", true}})["result"]["skipped"], true);
    EXPECT_EQ(call("notes.compressStart", {{"id", id}})["error"]["code"], "invalid_argument");
    generated = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(generated["ok"]) << generated;
    ASSERT_TRUE(call("session.delete", summary_identity(source))["ok"]);
    EXPECT_FALSE(call("sessionSummary.commit", {{"run_id", generated["result"]["run_id"]}, {"document", summary_document("Too late")}})["ok"]);
    const auto items = application_->list_note_items(epoch_);
    const auto item = std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.id == id; });
    ASSERT_NE(item, items.end());
    EXPECT_TRUE(item->is_summary);
    EXPECT_FALSE(item->summary_linked);
    EXPECT_TRUE(call("notes.rename", {{"id", id}, {"name", "Detached"}})["ok"]);
    EXPECT_EQ(call("notes.compressStart", {{"id", id}, {"skip_ineligible", true}})["result"]["skipped"], true);
    server.join();
}

TEST_F(BridgeRouterTest, SummaryCancellationRejectsReusedSourceIdentityAndVaultEpoch) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    (void)application_->save_notes_settings({"test"}, epoch_); // Deterministic echo provider.
    auto generated = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(generated["ok"]) << generated;
    ASSERT_TRUE(call("session.delete", summary_identity(source))["ok"]);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.prepare("INSERT INTO sessions(forum_key,session_id,label,updated_at,history_epoch,next_entry_id,next_request_id) "
            "SELECT forum_key,?,'Reused identity',0,1,1,1 FROM forums WHERE forum_id='lobby'", source.session_id).run();
    }
    const auto late = call("sessionSummary.commit", {{"run_id", generated["result"]["run_id"]}, {"document", summary_document("Old source")}});
    EXPECT_FALSE(late["ok"]);
    EXPECT_EQ(late["error"]["code"], "operation_cancelled");
    EXPECT_EQ(application_->list_note_items(epoch_).size(), 2U);
    const auto new_source = summary_source(*application_, database_, epoch_);
    const auto other = application_->create_vault(VaultCreate{.display_name="Summary other", .copy_from="Test", .password={}}, epoch_);
    generated = call("sessionSummary.generate", summary_identity(new_source));
    ASSERT_TRUE(generated["ok"]) << generated;
    const auto id = generated["result"]["run_id"].get<std::int64_t>();
    const auto switched = application_->switch_vault(other.name, {}, epoch_);
    EXPECT_THROW((void)application_->commit_session_summary(connection_, id, summary_document("Old vault"), epoch_), app::ApplicationError);
    EXPECT_THROW((void)application_->commit_session_summary(connection_, id, summary_document("Old vault"), switched.context_epoch), app::ApplicationError);
    EXPECT_EQ(application_->list_note_items(switched.context_epoch).size(), 2U);
    const auto back = application_->switch_vault("Test", {}, switched.context_epoch);
    EXPECT_EQ(application_->list_note_items(back.context_epoch).size(), 2U);
}

TEST_F(BridgeRouterTest, SummaryRetiredDuringRetryWaitStillSettlesItsReply) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    MockHttpServer server({"HTTP/1.1 429 Limited\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto other = application_->create_vault(VaultCreate{.display_name="Summary retry", .copy_from="Test", .password={}}, epoch_);
    // Vault maintenance retires the run while the worker waits to retry.
    compression_wait_ = [&](int) { (void)application_->switch_vault(other.name, {}, epoch_); };
    const auto generated = call("sessionSummary.generate", summary_identity(source));
    compression_wait_ = {};
    EXPECT_FALSE(generated["ok"]);
    EXPECT_EQ(generated["error"]["code"], "vault_changed");
    server.join();
}

TEST_F(BridgeRouterTest, SummaryConnectionCloseCancelsProviderAndRetiresExactlyOnce) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    MockHttpServer server({blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto request = next_id_++;
    router_->handle_request(connection_, request_json(connection_, request, epoch_, "sessionSummary.generate", summary_identity(source)).dump());
    router_->run_tasks();
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    router_->close_connection(connection_);
    server.join();
    EXPECT_EQ(application_->list_note_items(epoch_).size(), 2U);
    for (const auto deadline = std::chrono::steady_clock::now() + 5s;
         summary_events("Summary save cancelled") == 0 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::sleep_for(2ms);
    EXPECT_EQ(summary_events("Summary save cancelled"), 1U);
    connection_ = router_->open_connection();
    (void)application_->save_notes_settings({"test"}, epoch_);
    nlohmann::json provider = application_->get_provider("test", epoch_);
    for (const char* key : {"id", "used_by", "writable"}) provider.erase(key);
    provider["mode"] = "test";
    provider["api_key"] = "obsolete-key";
    (void)application_->update_provider("test", provider, epoch_);
    const auto next = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(next["ok"]) << next;
    EXPECT_TRUE(call("sessionSummary.abandon", {{"run_id", next["result"]["run_id"]}, {"reason", "conversion_failed"}})["ok"]);
}

TEST_F(BridgeRouterTest, SummaryConnectionCloseDoesNotWaitForTheLifecycleLock) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    (void)application_->save_notes_settings({"test"}, epoch_); // Deterministic echo provider.
    const auto ready = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(ready["ok"]) << ready;
    const auto note = compression_source(*application_, epoch_);
    std::atomic_bool entered{};
    std::future<void> close;
    bool returned = false;
    application_->set_note_compression_changed([&](auto, auto) {
        if (entered.exchange(true)) return;
        // The acceptance notice runs under the lifecycle lock, as a vault transfer does.
        close = std::async(std::launch::async, [&] { router_->close_connection(connection_); });
        returned = close.wait_for(2s) == std::future_status::ready;
    });
    (void)application_->compress_note(note.id, epoch_, false);
    close.get();
    EXPECT_TRUE(returned);
    wait_compression(note.id);
    application_->set_note_compression_changed({});
    // Cleanup still retires the closed window's ready run after the lock is free.
    connection_ = router_->open_connection();
    const auto next = generate_summary_after_cleanup(summary_identity(source));
    ASSERT_TRUE(next["ok"]) << next;
    EXPECT_FALSE(call("sessionSummary.commit", {{"run_id", ready["result"]["run_id"]}, {"document", summary_document("Stale")}})["ok"]);
    EXPECT_TRUE(call("sessionSummary.abandon", {{"run_id", next["result"]["run_id"]}, {"reason", "conversion_failed"}})["ok"]);
}

TEST_F(BridgeRouterTest, SummaryCanGenerateWhileBulkCompressionWaitsAndRejectsInvalidSources) {
    bootstrap_epoch();
    const auto source = summary_source(*application_, database_, epoch_);
    const auto note = compression_source(*application_, epoch_);
    MockHttpServer blocked({blocked_compression_response}, true);
    blocked.start();
    configure_compression(*application_, blocked.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(blocked.wait_for_requests(1, 2s));
    MockHttpServer summary({compression_response("chat_completions", false, "Short"),
        compression_response("chat_completions", false, "Summary")});
    summary.start();
    configure_compression(*application_, summary.port(), "chat_completions", false, epoch_);
    const auto generated = call("sessionSummary.generate", summary_identity(source));
    ASSERT_TRUE(generated["ok"]) << generated;
    ASSERT_TRUE(call("sessionSummary.abandon", {{"run_id", generated["result"]["run_id"]}, {"reason", "conversion_failed"}})["ok"]);
    const auto empty = application_->create_session("lobby", "Empty", epoch_);
    expect_note_error(call("sessionSummary.generate", {{"forum_id", "lobby"}, {"session_id", empty.id}}),
        "invalid_argument", "This session has no saved messages to summarize.");
    expect_note_error(call("sessionSummary.generate", {{"forum_id", "entrance"}, {"session_id", "welcome"}}),
        "invalid_argument", "Save summary is only available for configured forums.");
    EXPECT_FALSE(call("sessionSummary.generate", {{"forum_id", "lobby"}, {"session_id", source.session_id}, {"extra", true}})["ok"]);
    EXPECT_FALSE(call("sessionSummary.abandon", {{"run_id", 0}, {"reason", "conversion_failed"}})["ok"]);
    EXPECT_FALSE(call("sessionSummary.abandon", {{"run_id", 1}, {"reason", "arbitrary text"}})["ok"]);
    const auto items = application_->list_note_items(epoch_);
    for (const auto& item : items) if (item.forum_id)
        expect_note_error(call("notes.storeUpload", {{"description", ""}, {"folder_id", item.id}, {"service", "openai"}, {"api_key_id", "unused"}}),
            "invalid_argument", "This folder is managed by CHA.");
    ASSERT_TRUE(call("notes.compressCancel", {{"id", note.id}})["ok"]);
    summary.join();
    blocked.join();
}

TEST_F(BridgeRouterTest, NoteSummaryPlanChecksSizeAndFiltersFolderTargets) {
    bootstrap_epoch();
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Book", epoch_);
    const auto nested = application_->create_note_item(folder.id, NoteItemKind::folder, "Nested", epoch_);
    const auto create = [&](std::int64_t parent, std::string name, std::size_t size) {
        auto note = application_->create_note_item(parent, NoteItemKind::note, std::move(name), epoch_);
        application_->save_note(note.id, text_document(std::string(size, 'a')), epoch_);
        return note.id;
    };
    const auto short_note = create(folder.id, "Short", 4095);
    const auto min_note = create(folder.id, "Minimum", 4096);
    const auto max_note = create(folder.id, "Maximum", 262144);
    const auto long_note = create(folder.id, "Long", 262145);
    const auto excluded = create(folder.id, "Excluded", 4096);
    create(nested.id, "Descendant", 4096);
    ASSERT_TRUE(call("notes.setSummaryExcluded", {{"id", excluded}, {"excluded", true}})["ok"]);
    const auto plan = [&](std::int64_t id) { return call("notes.summaryPlan", {{"id", id}}); };
    expect_note_error(plan(short_note), "invalid_argument", "This note is under 4 KiB and does not need summaries.");
    expect_note_error(plan(long_note), "invalid_argument",
        "This note is over 256 KiB. Summaries are limited to 256 KiB of text.");
    expect_note_error(plan(min_note), "invalid_argument", "Select a compression provider in Notes settings.");
    expect_note_error(plan(folder.id), "invalid_argument", "Select a compression provider in Notes settings.");
    expect_note_error(plan(987654), "not_found", "That note or folder was not found.");

    (void)application_->save_notes_settings({"test"}, epoch_);
    ASSERT_TRUE(call("notes.summaryCommit", {{"id", min_note}, {"part", "l1"}, {"document", text_document("L1")}})["ok"]);
    ASSERT_TRUE(call("notes.summaryCommit", {{"id", max_note}, {"part", "l2"}, {"document", text_document("L2")}})["ok"]);
    ASSERT_TRUE(call("notes.summaryCommit",
        {{"id", folder.id}, {"part", "l3"}, {"document", text_document(std::string(4096, 's'))}})["ok"]);
    const auto events_before = call("events.list")["result"];
    const auto items = application_->list_note_items(epoch_);
    const auto summary = std::ranges::find_if(items, [](const auto& item) { return item.folder_summary; });
    ASSERT_NE(summary, items.end());
    expect_note_error(plan(summary->id), "invalid_argument", "That item is not a note.");
    const auto managed = std::ranges::find_if(items, [](const auto& item) { return item.forum_id.has_value(); });
    ASSERT_NE(managed, items.end());
    expect_note_error(plan(managed->id), "invalid_argument", "This folder is managed by CHA.");

    const nlohmann::json minimum{{"id", min_note}, {"name", "Minimum"}, {"has_l1", true}, {"has_l2", false}};
    const nlohmann::json maximum{{"id", max_note}, {"name", "Maximum"}, {"has_l1", false}, {"has_l2", true}};
    const nlohmann::json large{{"id", long_note}, {"name", "Long"}, {"has_l1", false}, {"has_l2", false}};
    EXPECT_EQ(plan(min_note)["result"]["notes"], nlohmann::json::array({minimum}));
    EXPECT_EQ(plan(max_note)["result"]["notes"], nlohmann::json::array({maximum}));
    EXPECT_EQ(plan(excluded)["result"]["notes"].size(), 1U);
    EXPECT_EQ(plan(folder.id)["result"], (nlohmann::json{{"notes", nlohmann::json::array({minimum, maximum, large})}}));
    EXPECT_EQ(call("events.list")["result"], events_before);
}

TEST_F(BridgeRouterTest, NoteSummaryMethodsRequireExactKeysPartsAndEpoch) {
    bootstrap_epoch();
    using Json = nlohmann::json;
    const auto note = compression_source(*application_, epoch_);
    const auto id = note.id;
    const std::pair<std::string_view, Json> requests[] = {
        {"notes.summaryPlan", {{"id", id}}},
        {"notes.summaryGenerate", {{"id", id}, {"part", "l1"}}},
        {"notes.summaryCommit", {{"id", id}, {"part", "l1"}, {"document", text_document("L1")}}},
        {"notes.summaryAbandon", {{"id", id}, {"part", "l2"}}},
        {"notes.summaryCancel", Json::object()},
        {"notes.setSummaryExcluded", {{"id", id}, {"excluded", true}}},
    };
    const auto invalid = [&](std::string_view method, const Json& params) {
        EXPECT_EQ(call(method, params)["error"]["code"], "invalid_argument") << method << " " << params;
    };
    for (const auto& [method, params] : requests) {
        auto extra = params;
        extra["extra"] = true;
        invalid(method, extra);
        for (auto key = params.begin(); key != params.end(); ++key) {
            auto missing = params;
            missing.erase(key.key());
            invalid(method, missing);
        }
    }
    for (const auto& part : {Json("l3"), Json("unknown"), Json(1)}) {
        invalid("notes.summaryGenerate", {{"id", id}, {"part", part}});
    }
    for (const auto part : {"l1", "l2", "l3_section"}) {
        invalid("notes.summaryGenerate", {{"id", id}, {"part", part}, {"text", "unexpected"}});
    }
    invalid("notes.summaryGenerate", {{"id", id}, {"part", "l3_overview"}});
    for (const auto& text : {Json(""), Json(123), Json(nullptr)}) {
        invalid("notes.summaryGenerate", {{"id", id}, {"part", "l3_overview"}, {"text", text}});
    }
    for (const auto& part : {Json("l3_section"), Json("l3_overview"), Json("unknown"), Json(nullptr)}) {
        invalid("notes.summaryCommit", {{"id", id}, {"part", part}, {"document", text_document("L1")}});
        invalid("notes.summaryAbandon", {{"id", id}, {"part", part}});
    }
    invalid("notes.summaryCommit", {{"id", id}, {"part", "l1"}, {"document", Json::array()}});
    invalid("notes.setSummaryExcluded", {{"id", id}, {"excluded", "true"}});
    invalid("notes.get", {{"id", id}, {"mode", "l3"}});
    const auto events = call("events.list")["result"]["items"];
    EXPECT_TRUE(events.empty());
    ++epoch_;
    for (const auto& [method, params] : requests)
        EXPECT_EQ(call(method, params)["error"]["code"], "vault_changed") << method;
}

TEST_F(BridgeRouterTest, NoteSummaryGenerateUsesPartPromptsAndSavedSource) {
    router_.reset();
    router_ = std::make_unique<BridgeRouter>(*application_,
        BridgeRouter::Options{.platform = "test", .command_deadline = 0ms});
    connection_ = router_->open_connection();
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Chapter", epoch_);
    application_->save_note(note.id, text_document("Original Markdown"), epoch_);
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Book", epoch_);
    const auto response = compression_response("chat_completions", false, "Generated Markdown");
    MockHttpServer server({response, response, response, response, response});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const std::array parts{"l1", "l2", "l3_section", "l3_overview"};
    const std::array prompts{"summary-l1", "summary-l2", "summary-l3-section", "summary-l3-overview"};
    for (const auto* prompt : prompts) (void)application_->update_prompt(prompt, {prompt}, epoch_);
    const auto generate = [&](nlohmann::json params) {
        const auto reply = call("notes.summaryGenerate", std::move(params));
        ASSERT_TRUE(reply.value("ok", false)) << reply;
        EXPECT_EQ(reply["result"], (nlohmann::json{{"markdown", "Generated Markdown"}}));
    };
    const auto first = queue_request(*router_, connection_, next_id_, epoch_, "notes.summaryGenerate",
        {{"id", note.id}, {"part", "l3_section"}});
    router_->expire_timeouts();
    const auto replies = wait_replies(*router_, connection_, {first});
    ASSERT_EQ(replies.size(), 1U);
    EXPECT_EQ(replies.at(first)["result"], (nlohmann::json{{"markdown", "Generated Markdown"}}));
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note.id, "Compressed Markdown");
    }
    for (const auto* part : parts) {
        if (std::string_view(part) == "l3_overview")
            generate({{"id", folder.id}, {"part", part}, {"text", "Sections Markdown"}});
        else generate({{"id", note.id}, {"part", part}});
    }
    server.join();
    const std::array expected_prompts{prompts[2], prompts[0], prompts[1], prompts[2], prompts[3]};
    const std::array expected_text{"Original Markdown", "Original Markdown", "Original Markdown",
        "Compressed Markdown", "Sections Markdown"};
    ASSERT_EQ(server.requests().size(), expected_prompts.size());
    for (std::size_t i = 0; i < expected_prompts.size(); ++i) {
        const auto body = nlohmann::json::parse(request_body(server.requests()[i]));
        EXPECT_EQ(body["messages"][0]["content"], expected_prompts[i]);
        EXPECT_EQ(body["messages"][1]["content"], std::string("from User:\n") + expected_text[i]);
        EXPECT_FALSE(body.contains("tools"));
        EXPECT_FALSE(body.contains("service_tier"));
    }
    ASSERT_TRUE(call("notes.summaryCancel")["ok"]);
    EXPECT_TRUE(call("events.list")["result"]["items"].empty());
}

TEST_F(BridgeRouterTest, NoteSummaryGenerateRecordsInputSizeErrors) {
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Chapter", epoch_);
    const std::string large(262145, 'a');
    application_->save_note(note.id, text_document(large), epoch_);
    const auto target = " for “Chapter” (note " + std::to_string(note.id) + "): ";
    const auto check = [&](std::string_view part, std::string_view label, std::string reason, bool section) {
        expect_note_error(call("notes.summaryGenerate", {{"id", note.id}, {"part", part}}),
            "invalid_argument", reason);
        const auto item = call("events.list")["result"]["items"].front();
        EXPECT_EQ(item["severity"], "error");
        EXPECT_EQ(item["message"], "Could not make " + std::string(label) + target + reason
            + (section ? ". The Summary note was not changed." : "."));
    };
    check("l1", "L1", "the note has more than 256 KiB of text", false);
    check("l2", "L2", "the note has more than 256 KiB of text", false);
    check("l3_section", "the L3 section",
        "the note has more than 256 KiB of text. Compress the note, then run Summarize again", true);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note.id, large);
    }
    check("l3_section", "the L3 section", "the compressed text has more than 256 KiB", true);
    EXPECT_EQ(call("events.list")["result"]["items"].size(), 4U);
    expect_note_error(call("notes.summaryGenerate", {{"id", note.id}, {"part", "l3_overview"}, {"text", "Overview"}}),
        "invalid_argument", "That item is not a folder.");
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Book", epoch_);
    expect_note_error(call("notes.summaryGenerate", {{"id", folder.id}, {"part", "l1"}}),
        "invalid_argument", "That item is not a note.");
    expect_note_error(call("notes.summaryGenerate", {{"id", folder.id}, {"part", "l3_overview"},
        {"text", std::string(note_document_byte_limit + 1, 'a')}}),
        "body_too_large", "The overview text exceeds 12 MiB.");
    const auto overview_event = call("events.list")["result"]["items"].front();
    EXPECT_EQ(overview_event["severity"], "error");
    EXPECT_EQ(overview_event["message"], "Could not make the L3 overview for “Book” (folder "
        + std::to_string(folder.id) + "): The overview text exceeds 12 MiB. The Summary note was not changed.");
}

TEST_F(BridgeRouterTest, NoteSummaryRetriesAndReportsSafeProviderErrors) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    const std::string unavailable = "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const auto result = [](std::string_view text, bool incomplete = false) {
        return compression_response("chat_completions", false, text, incomplete);
    };
    const std::string context_limit = "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer server({unavailable, unavailable, unavailable, unavailable, result("Partial", true),
        compression_refusal("chat_completions", false, false), context_limit,
        result(std::string(note_document_byte_limit + 1, 'x')), result(" "), result("Success")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    std::vector<int> waits;
    compression_wait_ = [&](int seconds) { waits.push_back(seconds); };
    const auto generate = [&] { return call("notes.summaryGenerate", {{"id", note.id}, {"part", "l1"}}); };
    expect_note_error(generate(), "internal_error",
        "The provider request failed. Check the connection and provider settings, then try again.");
    EXPECT_EQ(waits, (std::vector<int>{10, 20, 40}));
    expect_note_error(generate(), "internal_error", "The provider returned an incomplete result.");
    expect_note_error(generate(), "internal_error", "The provider refused this request.");
    expect_note_error(generate(), "invalid_argument", "Prompt exceeds the model's context window.");
    expect_note_error(generate(), "body_too_large", "The result exceeds 12 MiB.");
    expect_note_error(generate(), "internal_error", "The provider returned no text.");
    EXPECT_EQ(generate()["result"]["markdown"], "Success");
    ASSERT_TRUE(call("notes.summaryCancel")["ok"]);
    const auto events = call("events.list")["result"]["items"];
    ASSERT_EQ(events.size(), 6U);
    EXPECT_EQ(events.front()["severity"], "error");
    EXPECT_EQ(events.front()["message"], "Could not make L1 for “Source” (note " + std::to_string(note.id)
        + "): The provider returned no text.");
    server.join();
}

TEST_F(BridgeRouterTest, NoteSummaryStartupFailureKeepsStorageDetailsOutOfEvents) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    const auto generate = [&](std::int64_t id) {
        return call("notes.summaryGenerate", {{"id", id}, {"part", "l1"}});
    };
    expect_note_error(generate(987654), "not_found", "That note or folder was not found.");
    auto event = call("events.list")["result"]["items"].front();
    EXPECT_EQ(event["message"], "Could not make L1 for note 987654: That note or folder was not found.");

    storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
    database.execute("ALTER TABLE note_audio RENAME TO unavailable_note_audio");
    EXPECT_EQ(generate(note.id)["error"]["code"], "internal_error");
    event = call("events.list")["result"]["items"].front();
    EXPECT_EQ(event["severity"], "error");
    EXPECT_EQ(event["message"], "Could not make L1 for note " + std::to_string(note.id)
        + ": Could not start summary generation. Try again.");
}

TEST_F(BridgeRouterTest, NoteSummaryCommitAbandonAndCompressionPreserveVersions) {
    bootstrap_epoch();
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Book", epoch_);
    const auto note = application_->create_note_item(folder.id, NoteItemKind::note, "Chapter", epoch_);
    application_->save_note(note.id, text_document(std::string(4096, 'a')), epoch_);
    const auto expect_event = [&](std::string_view severity, std::string_view message) {
        const auto events = call("events.list")["result"]["items"];
        ASSERT_FALSE(events.empty());
        EXPECT_EQ(events.front()["severity"], severity);
        EXPECT_EQ(events.front()["message"], message);
    };
    const auto note_target = "“Chapter” (note " + std::to_string(note.id) + ")";
    const auto folder_target = "“Book” (folder " + std::to_string(folder.id) + ")";
    for (const auto& [mode, label] : {std::pair{"l1", "L1"}, std::pair{"l2", "L2"}}) {
        const auto doc = text_document(label);
        ASSERT_EQ(call("notes.summaryCommit",
            {{"id", note.id}, {"part", mode}, {"document", doc}})["result"], nlohmann::json::object());
        expect_event("success", std::string(label) + " ready for " + note_target + ".");
        ASSERT_EQ(call("notes.summaryAbandon", {{"id", note.id}, {"part", mode}})["result"], nlohmann::json::object());
        expect_event("error", "Could not make " + std::string(label) + " for " + note_target
            + ": the result could not be converted to a note.");
        EXPECT_EQ(call("notes.get", {{"id", note.id}, {"mode", mode}})["result"]["document"], doc);
        expect_note_error(call("notes.summaryCommit",
            {{"id", note.id}, {"part", mode}, {"document", nlohmann::json::object()}}),
            "invalid_argument", "Summary content must have a doc root.");
        expect_event("error", "Could not make " + std::string(label) + " for " + note_target
            + ": Summary content must have a doc root.");
        EXPECT_EQ(call("notes.get", {{"id", note.id}, {"mode", mode}})["result"]["document"], doc);
    }

    const auto l3 = [&](nlohmann::json document) {
        return call("notes.summaryCommit", {{"id", folder.id}, {"part", "l3"}, {"document", std::move(document)}});
    };
    ASSERT_EQ(l3(text_document("First folder summary"))["result"], nlohmann::json::object());
    const auto items = application_->list_note_items(epoch_);
    const auto summary = std::ranges::find_if(items, [](const auto& item) { return item.folder_summary; });
    ASSERT_NE(summary, items.end());
    EXPECT_TRUE(summary->is_summary);
    EXPECT_TRUE(summary->read_only);
    expect_event("success", "Summary ready for “Book” (folder " + std::to_string(folder.id)
        + ", note " + std::to_string(summary->id) + ").");
    const auto request = next_id_++;
    router_->handle_request(connection_, request_json(connection_, request, epoch_, "notes.summaryCommit",
        {{"id", folder.id}, {"part", "l3"}, {"document", text_document("Replacement folder summary")}}).dump());
    nlohmann::json reply;
    nlohmann::json notice;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while ((reply.empty() || notice.empty()) && std::chrono::steady_clock::now() < deadline) {
        const auto delivery = wait_delivery(*router_, connection_);
        if (!delivery) continue;
        ack_delivery(*router_, connection_, *delivery);
        if (reply.empty()) reply = reply_with_id(*delivery, request);
        for (const auto& message : delivery->at("messages")) {
            if (message.value("event", "") == "noteAudio.changed") notice = message;
        }
    }
    ASSERT_FALSE(reply.empty());
    ASSERT_TRUE(reply.at("ok"));
    ASSERT_FALSE(notice.empty());
    EXPECT_EQ(notice.at("id"), summary->id);
    EXPECT_EQ(notice.at("context_epoch"), epoch_);
    ASSERT_TRUE(call("notes.summaryAbandon", {{"id", folder.id}, {"part", "l3"}})["ok"]);
    expect_event("error", "Could not save the Summary note for " + folder_target
        + ": the result could not be converted to a note.");
    expect_note_error(l3(nlohmann::json::object()), "invalid_argument", "The summary document has no readable text.");
    expect_event("error", "Could not save the Summary note for " + folder_target
        + ": The summary document has no readable text.");
    EXPECT_EQ(call("notes.get", {{"id", summary->id}})["result"]["document"], text_document("Replacement folder summary"));

    const auto event_count = application_->list_events(epoch_).size();
    EXPECT_EQ(call("notes.compressStart", {{"id", summary->id}, {"skip_ineligible", true}})["result"]["skipped"], true);
    EXPECT_EQ(call("notes.compressStart", {{"id", summary->id}})["error"]["code"], "invalid_argument");
    EXPECT_EQ(application_->list_events(epoch_).size(), event_count);
    MockHttpServer server({compression_response("chat_completions", false, "Compressed")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(compress_and_wait({{"id", note.id}, {"skip_ineligible", true}})["ok"]);
    server.join();
    EXPECT_EQ(call("notes.get", {{"id", note.id}, {"mode", "l1"}})["result"]["document"], text_document("L1"));
    EXPECT_EQ(call("notes.get", {{"id", note.id}, {"mode", "l2"}})["result"]["document"], text_document("L2"));
}

TEST_F(BridgeRouterTest, NoteSummaryCancelIsConnectionScopedAndAllowsParallelCalls) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    const auto other_connection = router_->open_connection();
    MockHttpServer compression({blocked_compression_response}, true);
    compression.start();
    configure_compression(*application_, compression.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(compression.wait_for_requests(1, 2s));

    MockHttpServer first({blocked_compression_response}, true);
    MockHttpServer second({blocked_compression_response}, true);
    MockHttpServer third({blocked_compression_response}, true);
    MockHttpServer other({compression_response("chat_completions", false, "Other connection")});
    other.pause_before_response(1);
    const auto start = [&](MockHttpServer& server, std::string_view part, const std::string& connection) {
        server.start();
        configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
        const auto id = queue_request(*router_, connection, next_id_, epoch_, "notes.summaryGenerate",
            {{"id", note.id}, {"part", part}});
        router_->run_ordinary_tasks();
        EXPECT_TRUE(server.wait_for_requests(1, 2s));
        return id;
    };
    const auto a = start(first, "l1", connection_);
    const auto b = start(second, "l2", connection_);
    const auto c = start(third, "l3_section", connection_);
    const auto d = start(other, "l1", other_connection);
    const auto cancel = queue_request(*router_, connection_, next_id_, epoch_, "notes.summaryCancel", nlohmann::json::object());
    const auto replies = wait_replies(*router_, connection_, {a, b, c, cancel});
    ASSERT_EQ(replies.size(), 4U);
    EXPECT_EQ(replies.at(cancel)["result"], nlohmann::json::object());
    for (auto id : {a, b, c}) EXPECT_EQ(replies.at(id)["error"]["code"], "operation_cancelled");
    first.join();
    second.join();
    third.join();
    other.resume_responses();
    const auto other_replies = wait_replies(*router_, other_connection, {d});
    ASSERT_EQ(other_replies.size(), 1U);
    EXPECT_EQ(other_replies.at(d)["result"]["markdown"], "Other connection");
    other.join();
    const auto events = call("events.list")["result"]["items"];
    for (const auto label : {"L1", "L2", "L3 section"}) {
        const auto message = std::string(label) + " cancelled for “Source” (note " + std::to_string(note.id) + ").";
        EXPECT_EQ(std::ranges::count_if(events, [&](const auto& event) {
            return event["severity"] == "info" && event["message"] == message;
        }), 1);
    }
    EXPECT_EQ(call("notes.compressionStatus", {{"id", note.id}})["result"]["pending"], true);
    ASSERT_TRUE(call("notes.compressCancel", {{"id", note.id}})["ok"]);
    compression.join();
}

TEST_F(BridgeRouterTest, NoteSummaryConnectionCloseAndVaultChangeCancelHeldCalls) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    const auto begin = [&](MockHttpServer& server, const std::string& connection) {
        server.start();
        configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
        const auto id = queue_request(*router_, connection, next_id_, epoch_, "notes.summaryGenerate",
            {{"id", note.id}, {"part", "l1"}});
        router_->run_ordinary_tasks();
        EXPECT_TRUE(server.wait_for_requests(1, 2s));
        return id;
    };
    MockHttpServer closed({blocked_compression_response}, true);
    MockHttpServer kept({compression_response("chat_completions", false, "Still open")});
    kept.pause_before_response(1);
    (void)begin(closed, connection_);
    const auto other = router_->open_connection();
    const auto kept_id = begin(kept, other);
    router_->close_connection(connection_);
    closed.join();
    kept.resume_responses();
    const auto kept_replies = wait_replies(*router_, other, {kept_id});
    ASSERT_EQ(kept_replies.size(), 1U);
    EXPECT_EQ(kept_replies.at(kept_id)["result"]["markdown"], "Still open");
    kept.join();
    const auto cancelled = "L1 cancelled for “Source” (note " + std::to_string(note.id) + ").";
    for (const auto deadline = std::chrono::steady_clock::now() + 5s;
         summary_events(cancelled) == 0 && std::chrono::steady_clock::now() < deadline;)
        std::this_thread::sleep_for(2ms);
    EXPECT_EQ(summary_events(cancelled), 1U);

    connection_ = router_->open_connection();
    const auto vault = application_->create_vault(
        VaultCreate{.display_name = "Other summary vault", .copy_from = "Test", .password = {}}, epoch_);
    MockHttpServer old_first({blocked_compression_response}, true);
    MockHttpServer old_second({blocked_compression_response}, true);
    const auto a = begin(old_first, connection_);
    const auto b = begin(old_second, other);
    const auto switched = application_->switch_vault(vault.name, {}, epoch_);
    epoch_ = switched.context_epoch;
    const auto first = wait_replies(*router_, connection_, {a});
    const auto second = wait_replies(*router_, other, {b});
    ASSERT_EQ(first.size(), 1U);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(first.at(a)["error"]["code"], "vault_changed");
    EXPECT_EQ(second.at(b)["error"]["code"], "vault_changed");
    old_first.join();
    old_second.join();
    EXPECT_EQ(summary_events(cancelled), 1U);
    epoch_ = application_->switch_vault("Test", {}, epoch_).context_epoch;
    EXPECT_EQ(summary_events(cancelled), 1U);
}

TEST_F(BridgeRouterTest, CompressionUsesSavedTextAndOmitsEmptyResponsesInstructions) {
    bootstrap_epoch();
    MockHttpServer server({compression_response("responses", false, "First"),
        compression_response("responses", false, "Second")});
    server.start();
    configure_compression(*application_, server.port(), "responses", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    for (const auto text : {" # Literal $$(file.md) \n", ""}) {
        (void)application_->update_prompt("note-compression", {text}, epoch_);
        ASSERT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
    }
    server.join();
    ASSERT_EQ(server.requests().size(), 2U);
    const auto first = nlohmann::json::parse(request_body(server.requests()[0]));
    const auto second = nlohmann::json::parse(request_body(server.requests()[1]));
    EXPECT_EQ(first["instructions"], " # Literal $$(file.md) \n");
    EXPECT_FALSE(second.contains("instructions"));
    EXPECT_EQ(first["input"], second["input"]);
}

TEST_F(BridgeRouterTest, CompressionSkipsShortMarkdownBeforeProviderValidation) {
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Source", epoch_);
    expect_note_error(compress_and_wait({{"id", note.id}}), "invalid_argument",
        "This note is under 4 KiB and does not need compression.");
    const auto short_document = text_document(std::string(4095, 'a'));
    ASSERT_GT(short_document.dump().size(), 4096U);
    application_->save_note(note.id, short_document, epoch_);
    expect_note_error(compress_and_wait({{"id", note.id}}), "invalid_argument",
        "This note is under 4 KiB and does not need compression.");

    MockHttpServer server({compression_response("chat_completions", false, "Compressed text")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note.id, "Existing compression");
    }
    expect_note_error(compress_and_wait({{"id", note.id}}), "invalid_argument",
        "This note is under 4 KiB and does not need compression.");
    EXPECT_EQ(stored_compression(database_, note.id), "Existing compression");
    EXPECT_EQ(application_->get_note(note.id, epoch_).document, short_document);

    // Count UTF-8 bytes, not characters, and allow exactly 4 KiB.
    std::string text;
    for (int i = 0; i < 2048; ++i) text += "é";
    ASSERT_EQ(text.size(), 4096U);
    application_->save_note(note.id, text_document(text), epoch_);
    ASSERT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
    server.join();
    ASSERT_EQ(server.requests().size(), 1U);
    EXPECT_EQ(stored_compression(database_, note.id), "Compressed text");
}

TEST_F(BridgeRouterTest, FolderCompressionSkipsStoredAndShortNotesBeforeProviderValidation) {
    bootstrap_epoch();
    const auto note = application_->create_note_item({}, NoteItemKind::note, "Source", epoch_);
    const auto compress = [&] { return compress_and_wait({{"id", note.id}, {"skip_ineligible", true}}); };
    auto reply = compress();
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"], nlohmann::json({{"skipped", true}}));
    application_->save_note(note.id, text_document(std::string(4095, 'a')), epoch_);
    reply = compress();
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["skipped"], true);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note.id, "Existing compression");
    }
    reply = compress();
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["skipped"], true);
    application_->save_note(note.id, text_document(std::string(4096, 'a')), epoch_);
    reply = compress();
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"]["skipped"], true);
    EXPECT_EQ(stored_compression(database_, note.id), "Existing compression");

    const auto eligible = application_->create_note_item({}, NoteItemKind::note, "Eligible", epoch_);
    application_->save_note(eligible.id, text_document(std::string(4096, 'a')), epoch_);
    MockHttpServer server({compression_response("chat_completions", false, "New compression")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    reply = compress_and_wait({{"id", eligible.id}, {"skip_ineligible", true}});
    ASSERT_TRUE(reply["ok"]);
    EXPECT_EQ(reply["result"], nlohmann::json::object());
    EXPECT_EQ(stored_compression(database_, eligible.id), "New compression");
    server.join();
    EXPECT_EQ(server.requests().size(), 1U);
    EXPECT_FALSE(compress_and_wait({{"id", eligible.id}, {"skip_ineligible", "true"}})["ok"]);
}

TEST_F(BridgeRouterTest, CompressionReportsProviderSettingsAndEmptyNotes) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    expect_note_error(compress_and_wait({{"id", note.id}}), "invalid_argument",
        "Select a compression provider in Notes settings.");
    application_->save_note(note.id, text_document(std::string(4096, ' ')), epoch_);
    expect_note_error(compress_and_wait({{"id", note.id}}), "invalid_argument",
        "This note has no text to compress.");
    application_->save_note(note.id, text_document("Original source" + std::string(4096, 'a')), epoch_);
    (void)application_->save_notes_settings({"test"}, epoch_);
    for (const auto& [api, model] : {std::pair{"chat_completions", "mistral-large-latest"},
            std::pair{"responses", "gpt-4o"}, std::pair{"responses", "gpt-4.1-mini"}}) {
        nlohmann::json provider = application_->get_provider("test", epoch_);
        for (const char* key : {"id", "used_by", "writable"}) provider.erase(key);
        provider["mode"] = "net";
        provider["api"] = api;
        provider["host"] = std::string_view(api) == "responses" ? "api.openai.com" : "api.mistral.ai";
        provider["model"] = model;
        (void)application_->update_provider("test", provider, epoch_);
        expect_note_error(compress_and_wait({{"id", note.id}}), "invalid_argument",
            "This compression provider's model does not support high reasoning effort.");
    }
    nlohmann::json provider = application_->get_provider("test", epoch_);
    for (const char* key : {"id", "used_by", "writable"}) provider.erase(key);
    provider["host"] = "127.0.0.1";
    provider["model"] = "compression-model";
    provider["api_key"] = "missing-key";
    (void)application_->update_provider("test", provider, epoch_);
    // The user can fix a missing key, so the reason must reach the dialog.
    expect_note_error(compress_and_wait({{"id", note.id}}), "invalid_argument",
        "API key 'missing-key' does not exist");
}

TEST_F(BridgeRouterTest, CompressionStatusIncludesOnlyJobsInTheRecursiveFolderSubtree) {
    bootstrap_epoch();
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Folder", epoch_);
    const auto nested = application_->create_note_item(folder.id, NoteItemKind::folder, "Nested", epoch_);
    const auto other = application_->create_note_item({}, NoteItemKind::folder, "Other", epoch_);
    const auto source = [&](std::int64_t parent, std::string name) {
        const auto note = application_->create_note_item(parent, NoteItemKind::note, std::move(name), epoch_);
        application_->save_note(note.id, text_document(std::string(4096, 'a')), epoch_);
        return note;
    };
    const auto a = source(nested.id, "A"), b = source(folder.id, "B");
    const auto c = source(other.id, "C"), idle = source(folder.id, "Idle");
    MockHttpServer first({blocked_compression_response, blocked_compression_response}, true);
    MockHttpServer second({blocked_compression_response}, true);
    first.start(); second.start();
    configure_compression(*application_, first.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", a.id}})["ok"]);
    ASSERT_TRUE(first.wait_for_requests(1, 2s));
    configure_compression(*application_, second.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
    EXPECT_FALSE(second.wait_for_requests(1, 150ms));
    const auto pending = [&](std::int64_t id) {
        const auto reply = call("notes.compressionStatus", {{"id", id}});
        EXPECT_TRUE(reply["ok"]);
        return reply["result"]["pending"].get<bool>();
    };
    EXPECT_TRUE(pending(folder.id));
    EXPECT_TRUE(pending(nested.id));
    EXPECT_TRUE(pending(a.id));
    EXPECT_FALSE(pending(other.id));
    EXPECT_FALSE(pending(idle.id));
    ASSERT_TRUE(call("notes.compressCancel", {{"id", a.id}})["ok"]);
    ASSERT_TRUE(second.wait_for_requests(1, 2s));
    EXPECT_FALSE(pending(nested.id));
    EXPECT_TRUE(pending(folder.id));
    configure_compression(*application_, first.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", c.id}})["ok"]);
    EXPECT_FALSE(first.wait_for_requests(2, 150ms));
    ASSERT_TRUE(call("notes.compressCancel", {{"id", b.id}})["ok"]);
    ASSERT_TRUE(first.wait_for_requests(2, 2s));
    // An unrelated job must not keep this folder disabled after its last job retires.
    EXPECT_FALSE(pending(folder.id));
    EXPECT_TRUE(pending(other.id));
    ASSERT_TRUE(call("notes.compressCancel", {{"id", c.id}})["ok"]);
    EXPECT_FALSE(pending(other.id));
    first.join(); second.join();
}

TEST_F(BridgeRouterTest, CompressionFolderStatusClearsAfterAResultIsCommitted) {
    bootstrap_epoch();
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Folder", epoch_);
    const auto note = application_->create_note_item(folder.id, NoteItemKind::note, "Source", epoch_);
    application_->save_note(note.id, text_document(std::string(4096, 'a')), epoch_);
    MockHttpServer server({compression_response("chat_completions", false, "Summary")});
    server.pause_before_response(1);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    EXPECT_EQ(call("notes.compressionStatus", {{"id", folder.id}})["result"], nlohmann::json({{"pending", true}}));
    server.resume_responses();
    wait_compression(note.id);
    EXPECT_EQ(call("notes.compressionStatus", {{"id", folder.id}})["result"], nlohmann::json({{"pending", false}}));
    EXPECT_EQ(stored_compression(database_, note.id), "Summary");
    server.join();
}

TEST_F(BridgeRouterTest, CompressionResolvesTheCurrentKeyBeforeItsRequest) {
    bootstrap_epoch();
    MockHttpServer server({compression_response("chat_completions", false, "Summary")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto key = application_->create_api_key({.display_name = "Compression", .value = "original-secret"}, epoch_);
    nlohmann::json provider = application_->get_provider("test", epoch_);
    for (const char* field : {"id", "used_by", "writable"}) provider.erase(field);
    provider["api_key"] = key.id;
    (void)application_->update_provider("test", provider, epoch_);
    const auto note = compression_source(*application_, epoch_);
    bool changed = false;
    application_->set_note_compression_changed([&](auto, auto) {
        if (changed) return;
        changed = true;
        // The acceptance notice runs under the lifecycle lock, before the worker
        // can send. Editing the store directly avoids reacquiring that lock.
        application_->store().apply_api_key_update(key.id, key.display_name, "replacement-secret", "none");
    });
    EXPECT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
    application_->set_note_compression_changed({});
    server.join();
    EXPECT_TRUE(changed);
    ASSERT_EQ(server.requests().size(), 1U);
    EXPECT_EQ(server.requests().front().find("original-secret"), std::string::npos);
    EXPECT_NE(server.requests().front().find("Authorization: Bearer replacement-secret"), std::string::npos);
    EXPECT_EQ(application_->store().snapshot()->find_api_key(key.id)->value, "replacement-secret");
}

TEST_F(BridgeRouterTest, WaitingCompressionUsesReplacementKeysAndNeverSubstitutesADeletedKey) {
    bootstrap_epoch();
    const auto key = application_->create_api_key({.display_name = "Compression", .value = "original-secret"}, epoch_);
    for (const bool deleted : {false, true}) {
        SCOPED_TRACE(deleted ? "deleted legacy key" : "replaced key");
        MockHttpServer first({blocked_compression_response}, true);
        MockHttpServer next({compression_response("chat_completions", false, "Summary")});
        first.start(); next.start();
        configure_compression(*application_, first.port(), "chat_completions", false, epoch_);
        auto config = application_->store().snapshot()->find_provider("test")->config;
        config.api_key_id = deleted ? "" : key.id;
        config.api_key_env = deleted ? key.display_name : "";
        application_->store().apply_provider_update("test", "Test", config);
        const auto a = application_->create_note_item({}, NoteItemKind::note, deleted ? "Active delete" : "Active replace", epoch_);
        application_->save_note(a.id, text_document(std::string(4096, 'a')), epoch_);
        const auto b = application_->create_note_item({}, NoteItemKind::note, deleted ? "Delete" : "Replace", epoch_);
        application_->save_note(b.id, text_document(std::string(4096, 'b')), epoch_);
        ASSERT_TRUE(call("notes.compressStart", {{"id", a.id}})["ok"]);
        ASSERT_TRUE(first.wait_for_requests(1, 2s));
        config.port = next.port();
        application_->store().apply_provider_update("test", "Test", config);
        ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
        EXPECT_FALSE(next.wait_for_requests(1, 50ms));
        if (deleted) {
            application_->api_keys().remove(key.id);
            (void)application_->create_api_key({.display_name = key.display_name, .value = "different-key"}, epoch_);
        } else application_->api_keys().replace(key.id, "replacement-secret", "none");
        ASSERT_TRUE(call("notes.compressCancel", {{"id", a.id}})["ok"]);
        wait_compression(b.id); first.join();
        if (deleted) {
            EXPECT_EQ(stored_compression(database_, b.id), "");
            EXPECT_EQ(application_->list_events(epoch_).front().severity, EventSeverity::error);
            EXPECT_FALSE(next.wait_for_requests(1, 50ms));
            // A fresh explicit start can use the new key; the waiting job could not.
            ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
            wait_compression(b.id);
        }
        next.join();
        ASSERT_EQ(next.requests().size(), 1u);
        EXPECT_NE(next.requests()[0].find(deleted ? "Bearer different-key" : "Bearer replacement-secret"), std::string::npos);
        EXPECT_EQ(next.requests()[0].find("original-secret"), std::string::npos);
    }
}

TEST_F(BridgeRouterTest, CompressionResolvesTheKeyAgainBeforeRetrying) {
    bootstrap_epoch();
    const auto key = application_->create_api_key({.display_name = "Compression", .value = "original-secret"}, epoch_);
    for (const bool deleted : {false, true}) {
        std::vector<std::string> responses{"HTTP/1.1 429 Limited\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"};
        if (!deleted) responses.push_back(compression_response("chat_completions", false, "Summary"));
        MockHttpServer server(std::move(responses));
        server.start(); configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
        auto config = application_->store().snapshot()->find_provider("test")->config;
        config.api_key_id = key.id;
        application_->store().apply_provider_update("test", "Test", config);
        const auto note = application_->create_note_item({}, NoteItemKind::note, deleted ? "Delete key" : "Replace key", epoch_);
        application_->save_note(note.id, text_document(std::string(4096, 'a')), epoch_);
        compression_wait_ = [&](int seconds) {
            EXPECT_EQ(seconds, 10);
            if (deleted) application_->api_keys().remove(key.id);
            else application_->api_keys().replace(key.id, "replacement-secret", "none");
        };
        ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
        wait_compression(note.id); server.join();
        EXPECT_EQ(server.requests().size(), deleted ? 1u : 2u);
        if (deleted) {
            EXPECT_EQ(stored_compression(database_, note.id), "");
            EXPECT_EQ(application_->list_events(epoch_).front().severity, EventSeverity::error);
        } else {
            EXPECT_NE(server.requests()[0].find("Bearer original-secret"), std::string::npos);
            EXPECT_NE(server.requests()[1].find("Bearer replacement-secret"), std::string::npos);
        }
    }
    compression_wait_ = {};
}

TEST_F(BridgeRouterTest, CompressionDiscardsChangedTextBeforeSendingOrRetrying) {
    bootstrap_epoch();
    MockHttpServer first({blocked_compression_response}, true);
    MockHttpServer next({"HTTP/1.1 429 Limited\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    first.start(); next.start();
    configure_compression(*application_, first.port(), "chat_completions", false, epoch_);
    const auto a = compression_source(*application_, epoch_);
    const auto b = application_->create_note_item({}, NoteItemKind::note, "Waiting", epoch_);
    application_->save_note(b.id, text_document(std::string(4096, 'b')), epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", a.id}})["ok"]);
    ASSERT_TRUE(first.wait_for_requests(1, 2s));
    configure_compression(*application_, next.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
    application_->save_note(b.id, text_document(std::string(4096, 'c')), epoch_);
    ASSERT_TRUE(call("notes.compressCancel", {{"id", a.id}})["ok"]);
    wait_compression(b.id); first.join();
    EXPECT_FALSE(next.wait_for_requests(1, 50ms));
    EXPECT_EQ(application_->list_events(epoch_).front().severity, EventSeverity::warning);
    compression_wait_ = [&](int) { application_->save_note(b.id, text_document(std::string(4096, 'd')), epoch_); };
    ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
    wait_compression(b.id); next.join();
    EXPECT_EQ(next.requests().size(), 1u);
    EXPECT_EQ(stored_compression(database_, b.id), "");
    EXPECT_EQ(application_->list_events(epoch_).front().severity, EventSeverity::warning);
    compression_wait_ = {};
}

TEST_F(BridgeRouterTest, CompressionAndAudioOnTheSameNoteRunIndependently) {
    bootstrap_epoch();
    MockHttpServer compression({compression_response("chat_completions", false, "Summary")});
    compression.start();
    configure_compression(*application_, compression.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    (void)application_->set_note_read_only(note.id, true, epoch_);
    const auto key = application_->create_api_key({.display_name = "Audio", .value = "audio-secret"}, epoch_);
    MockHttpServer audio({blocked_compression_response}, true);
    audio.start();
    (void)application_->save_notes_settings({.compression_provider = "test", .compression_reasoning_effort = "none",
        .audio_url = "http://127.0.0.1:" + std::to_string(audio.port()), .audio_api_key = key.id}, epoch_);
    (void)application_->start_background_note_audio(note.id, "Kore", {"Audio source"}, epoch_);
    ASSERT_TRUE(audio.wait_for_requests(1, 2s));
    EXPECT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
    compression.join();
    EXPECT_EQ(stored_compression(database_, note.id), "Summary");
    EXPECT_TRUE(application_->background_note_audio_status(note.id, epoch_)["pending"].get<bool>());
    // Cancelling an already committed compression must not affect audio either.
    application_->cancel_note_compression(note.id, epoch_);
    EXPECT_TRUE(application_->background_note_audio_status(note.id, epoch_)["pending"].get<bool>());
    (void)application_->set_note_read_only(note.id, false, epoch_);
    audio.join();
}

TEST_F(BridgeRouterTest, CompressionAcceptsLocallyAndCancelAllowsImmediateRestart) {
    bootstrap_epoch();
    MockHttpServer slow({compression_response("chat_completions", false, "Cancelled result")});
    slow.pause_before_response(1);
    slow.start();
    configure_compression(*application_, slow.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note.id, "Previous");
    }
    EXPECT_EQ(call("notes.compressStart", {{"id", note.id}})["result"], nlohmann::json({{"accepted", true}}));
    ASSERT_TRUE(slow.wait_for_requests(1, 2s));
    EXPECT_EQ(call("notes.compressionStatus", {{"id", note.id}})["result"], nlohmann::json({{"pending", true}}));
    EXPECT_EQ(call("notes.compressStart", {{"id", note.id}})["error"]["code"], "invalid_argument");
    EXPECT_EQ(call("notes.compressStart", {{"id", note.id}, {"skip_ineligible", true}})["result"], nlohmann::json({{"skipped", true}}));
    EXPECT_EQ(application_->list_events(epoch_).front().message, "Compression started for “Source” (note " + std::to_string(note.id) + ").");
    EXPECT_TRUE(call("notes.compressCancel", {{"id", note.id}})["ok"]);
    EXPECT_FALSE(application_->note_compression_status(note.id, epoch_)["pending"].get<bool>());
    EXPECT_EQ(stored_compression(database_, note.id), "Previous");

    MockHttpServer replacement({compression_response("chat_completions", false, "Replacement")});
    replacement.start();
    configure_compression(*application_, replacement.port(), "chat_completions", false, epoch_);
    EXPECT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    slow.resume_responses();
    slow.join();
    wait_compression(note.id);
    replacement.join();
    EXPECT_EQ(stored_compression(database_, note.id), "Replacement");
    EXPECT_TRUE(call("notes.compressCancel", {{"id", note.id}})["ok"]);
    const auto events = application_->list_events(epoch_);
    EXPECT_EQ(events.size(), 4U);
    EXPECT_EQ(events.front().severity, EventSeverity::success);
}

TEST_F(BridgeRouterTest, CompressionForDifferentNotesIsSerializedAndSurvivesConnectionLoss) {
    bootstrap_epoch();
    MockHttpServer slow({compression_response("chat_completions", false, "Slow result")});
    slow.pause_before_response(1);
    slow.start();
    configure_compression(*application_, slow.port(), "chat_completions", false, epoch_);
    const auto a = compression_source(*application_, epoch_);
    const auto b = application_->create_note_item({}, NoteItemKind::note, "Second", epoch_);
    application_->save_note(b.id, text_document(std::string(4096, 'b')), epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", a.id}})["ok"]);
    ASSERT_TRUE(slow.wait_for_requests(1, 2s));
    router_->close_connection(connection_);
    connection_ = router_->open_connection();
    MockHttpServer fast({compression_response("chat_completions", false, "Fast result")});
    fast.start();
    configure_compression(*application_, fast.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}, {"skip_ineligible", true}})["ok"]);
    EXPECT_FALSE(fast.wait_for_requests(1, 150ms));
    EXPECT_TRUE(application_->note_compression_status(b.id, epoch_)["pending"].get<bool>());
    EXPECT_TRUE(application_->note_compression_status(a.id, epoch_)["pending"].get<bool>());
    slow.resume_responses();
    wait_compression(a.id);
    slow.join();
    wait_compression(b.id);
    fast.join();
    EXPECT_EQ(stored_compression(database_, b.id), "Fast result");
    EXPECT_EQ(stored_compression(database_, a.id), "Slow result");
}

TEST_F(BridgeRouterTest, CompressionRetriesTemporaryRejectionsAndCommitsOnlyOnce) {
    bootstrap_epoch();
    const std::string unavailable = "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    const std::string limited = "HTTP/1.1 429 Limited\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer server({unavailable, limited, compression_response("chat_completions", false, "Summary")});
    std::vector<int> waits;
    compression_wait_ = [&](int seconds) { waits.push_back(seconds); };
    server.start(); configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    wait_compression(note.id); server.join();
    EXPECT_EQ(waits, (std::vector<int>{10, 20}));
    ASSERT_EQ(server.requests().size(), 3u);
    for (const auto& request : server.requests()) EXPECT_EQ(request_body(request), request_body(server.requests()[0]));
    EXPECT_EQ(stored_compression(database_, note.id), "Summary");
    const auto events = application_->list_events(epoch_);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events.front().severity, EventSeverity::success);
}

TEST_F(BridgeRouterTest, CompressionExhaustsThreeRetriesThenLetsTheNextNoteRun) {
    bootstrap_epoch();
    const std::string unavailable = "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer first({unavailable, unavailable, unavailable, unavailable});
    MockHttpServer next({compression_response("chat_completions", false, "Next summary")});
    first.pause_before_response(1); first.start(); next.start();
    configure_compression(*application_, first.port(), "chat_completions", false, epoch_);
    const auto a = compression_source(*application_, epoch_);
    const auto b = application_->create_note_item({}, NoteItemKind::note, "Next", epoch_);
    application_->save_note(b.id, text_document(std::string(4096, 'b')), epoch_);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, a.id, "Previous");
    }
    std::vector<int> waits;
    compression_wait_ = [&](int seconds) {
        waits.push_back(seconds);
        EXPECT_FALSE(next.wait_for_requests(1, 30ms));
    };
    ASSERT_TRUE(call("notes.compressStart", {{"id", a.id}})["ok"]);
    ASSERT_TRUE(first.wait_for_requests(1, 2s));
    configure_compression(*application_, next.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
    EXPECT_FALSE(next.wait_for_requests(1, 150ms));
    first.resume_responses();
    wait_compression(a.id); wait_compression(b.id); first.join(); next.join();
    EXPECT_EQ(waits, (std::vector<int>{10, 20, 40}));
    EXPECT_EQ(first.requests().size(), 4u);
    EXPECT_EQ(next.requests().size(), 1u);
    EXPECT_EQ(stored_compression(database_, a.id), "Previous");
    EXPECT_EQ(stored_compression(database_, b.id), "Next summary");
    const auto events = application_->list_events(epoch_);
    EXPECT_EQ(events.size(), 4u);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) {
        return event.severity == EventSeverity::error;
    }), 1);
}

TEST_F(BridgeRouterTest, CompressionWaitingCancellationAndRestartSendsOnlyTheReplacement) {
    bootstrap_epoch();
    MockHttpServer first({blocked_compression_response}, true);
    MockHttpServer next({compression_response("chat_completions", false, "Replacement")});
    first.start(); next.start();
    configure_compression(*application_, first.port(), "chat_completions", false, epoch_);
    const auto a = compression_source(*application_, epoch_);
    const auto b = application_->create_note_item({}, NoteItemKind::note, "Waiting", epoch_);
    application_->save_note(b.id, text_document(std::string(4096, 'b')), epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", a.id}})["ok"]);
    ASSERT_TRUE(first.wait_for_requests(1, 2s));
    configure_compression(*application_, next.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
    EXPECT_FALSE(next.wait_for_requests(1, 150ms));
    ASSERT_TRUE(call("notes.compressCancel", {{"id", b.id}})["ok"]);
    application_->save_note(b.id, text_document("Replacement " + std::string(4096, 'c')), epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
    ASSERT_TRUE(call("notes.compressCancel", {{"id", a.id}})["ok"]);
    wait_compression(b.id); first.join(); next.join();
    ASSERT_EQ(next.requests().size(), 1u);
    EXPECT_NE(request_body(next.requests()[0]).find("Replacement"), std::string::npos);
    EXPECT_EQ(stored_compression(database_, b.id), "Replacement");
}

TEST_F(BridgeRouterTest, CompressionVaultChangeCancelsActiveAndWaitingRequests) {
    bootstrap_epoch();
    MockHttpServer first({blocked_compression_response}, true);
    MockHttpServer next({});
    first.start(); next.start();
    configure_compression(*application_, first.port(), "chat_completions", false, epoch_);
    const auto a = compression_source(*application_, epoch_);
    const auto b = application_->create_note_item({}, NoteItemKind::note, "Waiting", epoch_);
    application_->save_note(b.id, text_document(std::string(4096, 'b')), epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", a.id}})["ok"]);
    ASSERT_TRUE(first.wait_for_requests(1, 2s));
    configure_compression(*application_, next.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", b.id}})["ok"]);
    (void)application_->update_vault("Test", {.display_name = "Test"}, epoch_);
    epoch_ = application_->context_epoch();
    first.join(); next.join();
    EXPECT_FALSE(application_->note_compression_status(a.id, epoch_)["pending"].get<bool>());
    EXPECT_FALSE(application_->note_compression_status(b.id, epoch_)["pending"].get<bool>());
    application_->request_shutdown();
    EXPECT_TRUE(application_->join_shutdown(2s));
    EXPECT_TRUE(next.requests().empty());
    EXPECT_EQ(stored_compression(database_, a.id), "");
    EXPECT_EQ(stored_compression(database_, b.id), "");
}

TEST_F(BridgeRouterTest, CompressionDiscardsChangedSourceAndDeletionCancels) {
    bootstrap_epoch();
    MockHttpServer server({compression_response("chat_completions", false, "Stale"),
        compression_response("chat_completions", false, "Deleted")});
    server.pause_before_response(1);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    application_->save_note(note.id, text_document("Changed" + std::string(4096, 'a')), epoch_);
    server.resume_responses();
    wait_compression(note.id);
    EXPECT_EQ(stored_compression(database_, note.id), "");
    EXPECT_EQ(application_->list_events(epoch_).front().severity, EventSeverity::warning);
    server.pause_before_response(2);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(2, 2s));
    application_->delete_note_item(note.id, epoch_);
    EXPECT_FALSE(application_->note_compression_status(note.id, epoch_)["pending"].get<bool>());
    server.resume_responses();
    server.join();
    EXPECT_EQ(application_->list_events(epoch_).front().severity, EventSeverity::info);
}

TEST_F(BridgeRouterTest, CompressionCannotSaveIntoAChangedVault) {
    bootstrap_epoch();
    MockHttpServer server({blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    const auto copy = application_->create_vault(VaultCreate{.display_name = "Copied", .copy_from = "Test", .password = {}}, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    (void)application_->switch_vault(copy.name, {}, epoch_);
    server.join();
    EXPECT_EQ(stored_compression(database_, note.id), "");
    EXPECT_FALSE(application_->note_compression_status(note.id, application_->context_epoch())["pending"].get<bool>());
    storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
    EXPECT_EQ(event_list(database).front().message, "Compression cancelled for “Source” (note " + std::to_string(note.id) + ").");
}

TEST_F(BridgeRouterTest, CompressionStartEventFailureRejectsBeforeSending) {
    bootstrap_epoch();
    MockHttpServer server({compression_response("chat_completions", false, "Summary")});
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.execute("CREATE TRIGGER reject_event BEFORE INSERT ON event_history BEGIN SELECT RAISE(ABORT,'event failure'); END");
    }
    EXPECT_EQ(call("notes.compressStart", {{"id", note.id}})["error"]["code"], "command_queue_full");
    EXPECT_FALSE(application_->note_compression_status(note.id, epoch_)["pending"].get<bool>());
    EXPECT_FALSE(server.wait_for_requests(1, 50ms));
    EXPECT_TRUE(application_->list_events(epoch_).empty());
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.execute("DROP TRIGGER reject_event");
    }
    // A subsequent start proves the server was actually listening during rejection.
    EXPECT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
    server.join();
    EXPECT_EQ(server.requests().size(), 1U);
}

TEST_F(BridgeRouterTest, CompressionRetiresWhenSuccessEventOrNotificationFails) {
    bootstrap_epoch();
    MockHttpServer server({compression_response("chat_completions", false, "Unsaved"),
        compression_response("chat_completions", false, "Saved")});
    server.pause_before_response(1);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note.id, "Previous");
        database.execute("CREATE TRIGGER reject_success BEFORE INSERT ON event_history WHEN NEW.severity='success' BEGIN SELECT RAISE(ABORT,'event failure'); END");
    }
    server.resume_responses();
    wait_compression(note.id);
    EXPECT_EQ(stored_compression(database_, note.id), "Previous");
    EXPECT_EQ(application_->list_events(epoch_).front().severity, EventSeverity::error);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.execute("DROP TRIGGER reject_success");
    }
    application_->set_note_compression_changed([](auto, auto) { throw std::runtime_error("Notification failed"); });
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    wait_compression(note.id);
    server.join();
    EXPECT_EQ(stored_compression(database_, note.id), "Saved");
    EXPECT_EQ(application_->list_events(epoch_).front().severity, EventSeverity::success);
}

TEST_F(BridgeRouterTest, CompressionCancellationRetiresEvenWhenItsEventFails) {
    bootstrap_epoch();
    MockHttpServer server({blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.execute("CREATE TRIGGER reject_event BEFORE INSERT ON event_history BEGIN SELECT RAISE(ABORT,'event failure'); END");
    }
    EXPECT_TRUE(call("notes.compressCancel", {{"id", note.id}})["ok"]);
    EXPECT_FALSE(application_->note_compression_status(note.id, epoch_)["pending"].get<bool>());
    server.join();
    EXPECT_EQ(stored_compression(database_, note.id), "");
}

TEST_F(BridgeRouterTest, CompressionRenameAndMovePreserveWorkWhileFolderDeletionCancelsIt) {
    bootstrap_epoch();
    MockHttpServer finite({compression_response("chat_completions", false, "Summary")});
    finite.pause_before_response(1);
    finite.start();
    configure_compression(*application_, finite.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Folder", epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(finite.wait_for_requests(1, 2s));
    (void)application_->rename_note_item(note.id, "Renamed", epoch_);
    EXPECT_THROW(application_->delete_note_item(999999, epoch_), app::ApplicationError);
    EXPECT_TRUE(application_->note_compression_status(note.id, epoch_)["pending"].get<bool>());
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.prepare("UPDATE note_items SET parent_id=? WHERE id=?", folder.id, note.id).run();
    }
    finite.resume_responses();
    wait_compression(note.id);
    finite.join();
    EXPECT_EQ(stored_compression(database_, note.id), "Summary");
    EXPECT_NE(application_->list_events(epoch_).front().message.find("“Source”"), std::string::npos);
    MockHttpServer blocked({blocked_compression_response}, true);
    blocked.start();
    configure_compression(*application_, blocked.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(blocked.wait_for_requests(1, 2s));
    application_->delete_note_item(folder.id, epoch_);
    EXPECT_FALSE(application_->note_compression_status(note.id, epoch_)["pending"].get<bool>());
    blocked.join();
}

TEST_F(BridgeRouterTest, SuccessfulDeleteDoesNotDependOnAFollowupListing) {
    bootstrap_epoch();
    const auto target = application_->create_note_item({}, NoteItemKind::folder, "Delete me", epoch_);
    const auto corrupt = application_->create_note_item({}, NoteItemKind::folder, "Damaged", epoch_);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.execute("PRAGMA ignore_check_constraints=ON");
        database.prepare("UPDATE note_items SET kind='broken' WHERE id=?", corrupt.id).run();
    }
    // A damaged unrelated row makes listing fail, while deleting this folder can succeed.
    EXPECT_THROW((void)application_->list_note_items(epoch_), app::ApplicationError);
    EXPECT_TRUE(call("notes.delete", {{"id", target.id}})["ok"]);
    storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
    auto remaining = database.prepare("SELECT id FROM note_items WHERE id=?", target.id);
    EXPECT_FALSE(remaining.step());
}

TEST_F(BridgeRouterTest, FailedDeletePreparationPreservesTheNoteAndItsCompression) {
    bootstrap_epoch();
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Folder", epoch_);
    const auto note = compression_source(*application_, epoch_);
    const auto corrupt = application_->create_note_item({}, NoteItemKind::folder, "Damaged", epoch_);
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.prepare("UPDATE note_items SET parent_id=? WHERE id=?", folder.id, note.id).run();
    }
    MockHttpServer server({blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        database.execute("PRAGMA ignore_check_constraints=ON");
        database.prepare("UPDATE note_items SET kind='broken' WHERE id=?", corrupt.id).run();
    }
    EXPECT_FALSE(call("notes.delete", {{"id", folder.id}})["ok"]);
    EXPECT_TRUE(application_->note_compression_status(note.id, epoch_)["pending"].get<bool>());
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        auto present = database.prepare("SELECT id FROM note_items WHERE id=?", folder.id);
        EXPECT_TRUE(present.step());
        database.prepare("UPDATE note_items SET kind='folder' WHERE id=?", corrupt.id).run();
    }
    EXPECT_TRUE(call("notes.delete", {{"id", folder.id}})["ok"]);
    EXPECT_FALSE(application_->note_compression_status(note.id, epoch_)["pending"].get<bool>());
    server.join();
}

TEST_F(BridgeRouterTest, CompressionConfigurationExportCancelsTheSameVaultContext) {
    bootstrap_epoch();
    MockHttpServer server({blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    // No export destination is configured. Even failed maintenance revokes jobs.
    try { (void)application_->export_configuration(epoch_); } catch (const std::exception&) {}
    server.join();
    EXPECT_EQ(stored_compression(database_, note.id), "");
    EXPECT_FALSE(application_->note_compression_status(note.id, application_->context_epoch())["pending"].get<bool>());
}

TEST_F(BridgeRouterTest, CompressionShutdownSignalsTheSharedWorkerControl) {
    bootstrap_epoch();
    MockHttpServer server({blocked_compression_response}, true);
    server.start();
    configure_compression(*application_, server.port(), "chat_completions", false, epoch_);
    const auto note = compression_source(*application_, epoch_);
    ASSERT_TRUE(call("notes.compressStart", {{"id", note.id}})["ok"]);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    application_->request_shutdown();
    EXPECT_TRUE(application_->join_shutdown(2s));
    server.join();
    EXPECT_EQ(stored_compression(database_, note.id), "");
    storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
    EXPECT_EQ(event_list(database).front().message, "Compression cancelled for “Source” (note " + std::to_string(note.id) + ").");
}

TEST_F(BridgeRouterTest, CompressionHasNoTransferLimitButKeepsTheIdleTimeout) {
    bootstrap_epoch();
    const auto note = compression_source(*application_, epoch_);
    MockHttpServer slow({"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n"
        + nlohmann::json{{"choices", nlohmann::json::array({{{"message", {{"content", "Slow compression"}}}, {"finish_reason", "stop"}}})}}.dump()}, false, 1200ms);
    slow.start();
    configure_compression(*application_, slow.port(), "chat_completions", false, epoch_, 1, 2);
    EXPECT_TRUE(compress_and_wait({{"id", note.id}})["ok"]);
    slow.join();
    EXPECT_EQ(stored_compression(database_, note.id), "Slow compression");
    MockHttpServer stalled({blocked_compression_response + ": waiting\n\n"}, true);
    stalled.start();
    configure_compression(*application_, stalled.port(), "chat_completions", false, epoch_, 1, 1);
    expect_note_error(compress_and_wait({{"id", note.id}}), "internal_error", "HTTP request failed: idle timeout reached");
    stalled.join();
    EXPECT_EQ(stored_compression(database_, note.id), "Slow compression");
}

TEST_F(BridgeRouterTest, ManagedForumFoldersRoundTripAndRejectDirectMutations) {
    bootstrap_epoch();
    const auto reply = call("notes.list");
    ASSERT_TRUE(reply["ok"]);
    const auto items = reply["result"]["items"];
    ASSERT_EQ(items.size(), 2u);
    for (const auto& item : items) {
        ASSERT_TRUE(item.contains("forum_id"));
        EXPECT_TRUE(item["forum_id"].is_string());
        EXPECT_EQ(item["kind"], "folder");
        const auto id = item["id"];
        expect_note_error(call("notes.create", {{"parent_id", id}, {"kind", "note"}, {"name", "Child"}}),
            "invalid_argument", "This folder is managed by CHA.");
        expect_note_error(call("notes.rename", {{"id", id}, {"name", item["name"]}}),
            "invalid_argument", "This folder is managed by CHA.");
        expect_note_error(call("notes.delete", {{"id", id}}),
            "invalid_argument", "This folder is managed by CHA.");
    }
    EXPECT_EQ(items[0]["forum_id"], "");
    EXPECT_EQ(items[1]["forum_id"], "lobby");
    expect_note_error(call("notes.create", {{"parent_id", nullptr}, {"kind", "folder"}, {"name", "Fake"}, {"forum_id", "fake"}}),
        "invalid_argument", "The request was not valid.");
}

TEST_F(BridgeRouterTest, NotesWireContract) {
    bootstrap_epoch();
    const auto empty = call("notes.list");
    ASSERT_TRUE(empty["ok"]);
    EXPECT_EQ(empty["result"]["items"].size(), 2u);

    const auto folder = call("notes.create", {
        {"parent_id", nullptr}, {"kind", "folder"}, {"name", "Plan"},
    });
    ASSERT_TRUE(folder["ok"]);
    EXPECT_EQ(folder["result"]["parent_id"], nullptr);
    EXPECT_EQ(folder["result"]["kind"], "folder");
    EXPECT_EQ(folder["result"]["name"], "Plan");
    EXPECT_EQ(folder["result"].size(), 14);
    EXPECT_EQ(folder["result"]["has_compressed"], false);
    EXPECT_EQ(folder["result"]["has_l1"], false);
    EXPECT_EQ(folder["result"]["has_l2"], false);
    EXPECT_EQ(folder["result"]["summary_excluded"], false);
    EXPECT_EQ(folder["result"]["folder_summary"], false);
    EXPECT_EQ(folder["result"]["has_audio"], false);
    EXPECT_EQ(folder["result"]["has_l1_audio"], false);
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
    EXPECT_EQ(loaded["result"]["document_id"], note_id);
    EXPECT_EQ(
        loaded["result"]["document"],
        nlohmann::json::parse(std::string(empty_note_document)));

    const auto document = nlohmann::json::parse(R"({"type":"doc","extra":true})");
    ASSERT_TRUE(call("notes.save", {{"id", note_id}, {"document", document}})["ok"]);
    ASSERT_TRUE(call("notes.save", {{"id", note_id}, {"document", document}})["ok"]);
    EXPECT_EQ(call("notes.get", {{"id", note_id}})["result"]["document"], document);

    EXPECT_EQ(call("notes.get", {{"id", note_id}, {"mode", "compressed"}})["result"]["document_id"], note_id);
    expect_note_error(call("notes.get", {{"id", note_id}, {"mode", "other"}}),
        "invalid_argument", "The note text mode is not valid.");
    EXPECT_FALSE(call("notes.get", {{"id", note_id}, {"mode", nullptr}})["ok"].get<bool>());
    std::int64_t compressed_id;
    {
        storage::SqliteDatabase database(database_, storage::SqliteDatabase::Mode::read_write);
        note_save_compressed(database, note_id, "Stored compressed text");
        compressed_id = database.last_insert_rowid();
    }
    const auto compressed = call("notes.get", {{"id", note_id}, {"mode", "compressed"}});
    ASSERT_TRUE(compressed["ok"]);
    EXPECT_EQ(compressed["result"], nlohmann::json({
        {"id", note_id}, {"document_id", compressed_id},
        {"document", note_document_from_text("Stored compressed text")}, {"scroll_top", 0}}));
    const auto edited = note_document_from_text("Edited compression");
    EXPECT_EQ(call("notes.save", {{"id", compressed_id}, {"document", edited}})["result"],
        nlohmann::json({{"annotation_created", false}}));
    EXPECT_TRUE(call("notes.saveScrollPosition", {{"id", compressed_id}, {"scroll_top", 12}})["ok"].get<bool>());
    EXPECT_EQ(call("notes.get", {{"id", note_id}, {"mode", "full_text"}})["result"]["document"], document);
    const auto reopened = call("notes.get", {{"id", note_id}, {"mode", "compressed"}})["result"];
    EXPECT_EQ(reopened["document"], edited);
    EXPECT_EQ(reopened["scroll_top"], 12);
    expect_note_error(call("notes.get", {{"id", compressed_id}}), "not_found", "That note was not found.");

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
    EXPECT_EQ(ordinary_note_items(call("notes.list")["result"]["items"])[0]["read_only"], true);
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
    const auto items = ordinary_note_items(call("notes.list")["result"]["items"]);
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
    EXPECT_EQ(ordinary_note_items(call("notes.list")["result"]["items"]).size(), 3U);
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
    const auto items = ordinary_note_items(call("notes.list")["result"]["items"]);
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
    EXPECT_EQ(ordinary_note_items(call("notes.list")["result"]["items"]), items);
}


TEST_F(BridgeRouterTest, AnnotationWireContractGuardsAndChatText) {
    bootstrap_epoch();
    const auto source = call("notes.create", {{"parent_id", nullptr}, {"kind", "note"}, {"name", "Source"}})["result"]["id"];
    const auto doc = nlohmann::json::parse(R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Passage","marks":[{"type":"highlight"}]}]}]})");
    const auto saved = call("notes.save", {{"id", source}, {"document", doc}});
    ASSERT_TRUE(saved["ok"]);
    EXPECT_EQ(saved["result"], nlohmann::json({{"annotation_created", true}}));
    EXPECT_EQ(call("notes.save", {{"id", source}, {"document", doc}})["result"], nlohmann::json({{"annotation_created", false}}));
    const auto items = ordinary_note_items(call("notes.list")["result"]["items"]);
    ASSERT_EQ(items.size(), 2u);
    const auto id = items[1]["id"];
    EXPECT_EQ(items[1]["kind"], "annotation");
    EXPECT_TRUE(call("notes.get", {{"id", id}})["ok"]);
    EXPECT_TRUE(call("notes.saveScrollPosition", {{"id", id}, {"scroll_top", 17}})["ok"]);
    for (const auto& method : {"notes.compressStart", "notes.save", "notes.rename", "notes.delete", "notes.setReadOnly"}) {
        auto params = nlohmann::json{{"id", id}};
        if (std::string_view(method) == "notes.save") params["document"] = doc;
        if (std::string_view(method) == "notes.rename") params["name"] = "Other";
        if (std::string_view(method) == "notes.setReadOnly") params["read_only"] = false;
        expect_note_error(call(method, params), "invalid_argument", "That item is not a note.");
    }
    EXPECT_EQ(compress_and_wait({{"id", id}, {"skip_ineligible", true}})["result"], nlohmann::json({{"skipped", true}}));
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


TEST_F(BridgeRouterTest, StoreMethodsRejectMalformedInputsAndListNoStoreLinks) {
    bootstrap_epoch();
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Research", epoch_);
    const auto child = application_->create_note_item(folder.id, NoteItemKind::folder, "Empty", epoch_);
    const auto key = application_->create_api_key({.display_name = "Saved", .value = "private-value"}, epoch_);
    EXPECT_EQ(call("notes.storeOperation", {{"folder_id", folder.id}})["result"], (nlohmann::json{{"present", false}}));
    auto listed = call("notes.list")["result"];
    listed["items"] = ordinary_note_items(listed["items"]);
    ASSERT_EQ(listed["items"].size(), 2u);
    for (const auto& item : listed["items"]) EXPECT_FALSE(item.contains("store"));
    EXPECT_EQ(listed.dump().find("private-value"), std::string::npos);
    for (const auto& [method, params] : std::vector<std::pair<std::string, nlohmann::json>>{
        {"notes.storeChoices", {{"unused", true}}},
        {"notes.storeUpload", {{"description", ""}, {"folder_id", child.id}, {"service", "openai"}, {"api_key_id", "bad/key"}}},
        {"notes.storeUpload", {{"description", ""}, {"folder_id", 0}, {"service", "openai"}, {"api_key_id", key.id}}},
        {"notes.storeUpload", {{"description", ""}, {"folder_id", child.id}, {"service", "other"}, {"api_key_id", key.id}}},
        {"notes.storeCleanup", {{"operation_id", 999}}},
        {"notes.storeCleanup", {{"operation_id", 1.5}}},
        {"notes.storeCleanup", {{"operation_id", 1}, {"folder_id", folder.id}}},
        {"notes.storeOperation", {{"folder_id", -1}}},
        {"notes.storeCancel", {{"operation_id", 999}}},
        {"notes.storeClose", {{"operation_id", 999}}},
    }) {
        SCOPED_TRACE(method + " " + params.dump());
        const auto reply = call(method, params);
        EXPECT_EQ(reply["ok"], false);
        EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    }
    EXPECT_EQ(ordinary_note_items(nlohmann::json(application_->list_note_items(epoch_))).size(), 2u);
    const auto current_epoch = epoch_;
    epoch_ += 1;
    EXPECT_EQ(call("notes.storeChoices")["error"]["code"], "vault_changed");
    epoch_ = current_epoch;
}

TEST_F(BridgeRouterTest, StoreUploadReportsEditablePreflightAndOrdinaryFolderDelete) {
    bootstrap_epoch();
    const auto fixture = folder_store_wire_fixture();
    const auto folder = application_->create_note_item({}, NoteItemKind::folder, "Empty", epoch_);
    const nlohmann::json metadata_request{{"folder_id", folder.id}, {"service", "openai"},
        {"api_key_id", "missing"}, {"description", ""}};
    for (const auto* field : {"description"}) {
        auto missing = metadata_request;
        missing.erase(field);
        EXPECT_EQ(call("notes.storeUpload", missing)["error"]["code"], "invalid_argument");
        for (const nlohmann::json& invalid : {nlohmann::json(nullptr), nlohmann::json(1), nlohmann::json::array()}) {
            auto wrong_type = metadata_request;
            wrong_type[field] = invalid;
            EXPECT_EQ(call("notes.storeUpload", wrong_type)["error"]["code"], "invalid_argument");
        }
    }
    auto unknown = metadata_request;
    unknown["extra"] = true;
    EXPECT_EQ(call("notes.storeUpload", unknown)["error"]["code"], "invalid_argument");
    EXPECT_FALSE(application_->folder_store_operation_status(folder.id, epoch_).present);
    const auto accepted = call("notes.storeUpload", {{"description", ""}, {"folder_id", folder.id}, {"service", "openai"}, {"api_key_id", "missing"}});
    ASSERT_EQ(accepted["ok"], true);
    nlohmann::json status;
    for (const auto deadline = std::chrono::steady_clock::now() + 3s; std::chrono::steady_clock::now() < deadline;) {
        status = call("notes.storeOperation", {{"folder_id", folder.id}})["result"];
        if (status.value("terminal", false)) break;
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(status["result"], "preflight");
    EXPECT_EQ(status["phase"], "failed");
    EXPECT_EQ(status["known_cleanup_remains"], false);
    EXPECT_FALSE(status["error"].get<std::string>().empty());
    const auto id = accepted["result"]["operation_id"];
    auto expected_start = fixture.at("start");
    expected_start["operation_id"] = id;
    EXPECT_EQ(accepted.at("result"), expected_start);
    auto expected_status = fixture.at("failed_upload");
    expected_status["operation_id"] = id;
    expected_status["completed"] = 0;
    expected_status["total"] = 0;
    expected_status["result"] = "preflight";
    expected_status["error"] = "The saved API key is missing or empty.";
    expected_status["known_cleanup_remains"] = false;
    EXPECT_EQ(status, expected_status);
    EXPECT_EQ(call("notes.storeCleanup", {{"operation_id", id}})["ok"], false);
    EXPECT_EQ(call("notes.storeClose", {{"operation_id", id}})["ok"], true);
    EXPECT_EQ(call("notes.storeOperation", {{"folder_id", folder.id}}).at("result"), fixture.at("no_operation"));
    EXPECT_EQ(call("notes.delete", {{"id", folder.id}})["ok"], true);
}


TEST_F(BridgeRouterTest, D1StorageBridgeValidatesAndHidesTokens) {
    bootstrap_epoch();
    auto reply = call("d1Storage.get");
    ASSERT_TRUE(reply["ok"]);
    EXPECT_TRUE(reply["result"].is_null());
    reply = call("d1Storage.save", {{"display_name", "D1"}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    reply = call(
        "d1Storage.save",
        {{"display_name", "D1"},
         {"account_id", ""},
         {"database_id", "d"},
         {"api_token", "t"}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    reply = call(
        "d1Storage.save",
        {{"display_name", "D1"},
         {"account_id", "a"},
         {"database_id", "d"},
         {"api_token", "t"},
         {"extra", true}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    // The fixture vault has no machine name, so a valid save is refused
    // before any network use.
    reply = call(
        "d1Storage.save",
        {{"display_name", "D1"},
         {"account_id", "a"},
         {"database_id", "d"},
         {"api_token", "t"}});
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "invalid_argument");
    EXPECT_EQ(reply["error"]["message"],
        "This machine has no name. Add machine to app.toml, then save again.");
    reply = call("d1Storage.delete");
    EXPECT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "not_found");
}

TEST_F(BridgeRouterTest, D1StorageBridgeSavesDisconnectsAndNotifiesNotes) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto command = make_command(workspace, database);
    command.machine = "mac";
    nlohmann::json incoming = nlohmann::json::array();
    for (const auto name : {"Incoming folder", "Another incoming folder"}) {
        const nlohmann::json record{
            {"kind", "note.create"}, {"path", {name}}, {"item", "folder"},
        };
        incoming.push_back({
            {"rowid", incoming.size() + 1}, {"piece", 1}, {"pieces", 1},
            {"record", record.dump()},
        });
    }
    std::atomic_bool fail_drop{false};
    D1Client::Transport transport =
        [&incoming, &fail_drop](const std::string&, const std::string& body,
            const std::vector<std::string>&) -> std::pair<long, std::string> {
        const auto batch = nlohmann::json::parse(body).at("batch");
        const auto sql = batch.at(0).at("sql").get<std::string>();
        nlohmann::json rows = nlohmann::json::array();
        if (sql.starts_with("SELECT name FROM sqlite_master")) {
            rows.push_back({{"name", "q/Test/mac"}});
        } else if (sql.starts_with("SELECT rowid")) {
            rows = incoming;
        } else if (sql.starts_with("DELETE FROM")) {
            incoming.clear();
        } else if (sql.starts_with("DROP TABLE") && fail_drop) {
            return {500, R"({"success":false})"};
        }
        const auto result = nlohmann::json::array({{{"success", true}, {"results", rows}}});
        return {200, nlohmann::json{{"success", true}, {"result", result}}.dump()};
    };
    auto application = app::Application::open(command, {}, {}, transport);
    auto now = std::chrono::steady_clock::now();
    BridgeRouter router(*application, {.command_deadline = 1s, .clock = [&] { return now; }});
    const std::string connection = router.open_connection();
    const auto epoch = application->context_epoch();
    std::uint64_t next_id = 1;
    int note_events = 0;
    auto receive = [&] {
        auto delivery = wait_delivery(router, connection);
        if (delivery) {
            for (const auto& message : delivery->at("messages")) {
                if (message.value("event", "") == "notes.changed") {
                    EXPECT_EQ(message.at("context_epoch"), epoch);
                    ++note_events;
                }
            }
            ack_delivery(router, connection, *delivery);
        }
        return delivery;
    };
    auto round_trip = [&](std::string_view method, nlohmann::json params) {
        const auto id = next_id++;
        router.handle_request(
            connection, request_json(connection, id, epoch, method, std::move(params)).dump());
        if (method != "d1Storage.get") {
            // D1's own transport timeouts can outlast the ordinary bridge deadline.
            now += 2s;
            router.expire_timeouts();
        }
        while (auto delivery = receive()) {
            auto reply = reply_with_id(*delivery, id);
            if (!reply.empty()) return reply;
        }
        ADD_FAILURE() << "No reply for " << method;
        return nlohmann::json{};
    };
    auto saved = round_trip(
        "d1Storage.save",
        {{"display_name", "D1"},
         {"account_id", "a"},
         {"database_id", "d"},
         {"api_token", "t"}});
    ASSERT_TRUE(saved["ok"]) << saved.dump();
    EXPECT_EQ(saved["result"]["account_id"], "a");
    EXPECT_TRUE(saved["result"]["has_api_token"].get<bool>());
    EXPECT_FALSE(saved["result"].contains("api_token"));
    if (note_events == 0) ASSERT_TRUE(receive());
    EXPECT_EQ(note_events, 1);
    const auto notes = application->list_note_items(epoch);
    EXPECT_NE(std::ranges::find(notes, "Incoming folder", &NoteItem::name), notes.end());
    EXPECT_NE(std::ranges::find(notes, "Another incoming folder", &NoteItem::name), notes.end());
    // Unchanged valid credentials are accepted and keep the saved token.
    auto unchanged = round_trip(
        "d1Storage.save",
        {{"display_name", "D1"},
         {"account_id", "a"},
         {"database_id", "d"},
         {"api_token", nullptr}});
    ASSERT_TRUE(unchanged["ok"]) << unchanged.dump();
    EXPECT_TRUE(unchanged["result"]["has_api_token"].get<bool>());
    auto loaded_detail = round_trip("d1Storage.get", nlohmann::json::object());
    ASSERT_TRUE(loaded_detail["ok"]);
    EXPECT_EQ(loaded_detail["result"]["database_id"], "d");
    EXPECT_TRUE(loaded_detail["result"]["has_api_token"].get<bool>());
    EXPECT_FALSE(loaded_detail["result"].contains("api_token"));
    // A remote drop failure still disconnects locally.
    fail_drop = true;
    auto removed = round_trip("d1Storage.delete", nlohmann::json::object());
    ASSERT_TRUE(removed["ok"]) << removed.dump();
    auto after = round_trip("d1Storage.get", nlohmann::json::object());
    ASSERT_TRUE(after["ok"]);
    EXPECT_TRUE(after["result"].is_null());
    EXPECT_EQ(note_events, 1);
    router.shutdown();
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST_F(BridgeRouterTest, StoreChoicesMatchTheSharedWireFixture) {
    bootstrap_epoch();
    const auto openai = application_->create_api_key(
        {.display_name = "OpenAI", .value = "openai-secret", .cloud_store = "openai"}, epoch_);
    const auto google = application_->create_api_key(
        {.display_name = "Google", .value = "google-secret", .cloud_store = "google"}, epoch_);
    auto actual = call("notes.storeChoices").at("result");
    auto expected = folder_store_wire_fixture().at("choices");
    for (auto& choice : expected["choices"])
        choice["api_key_id"] = choice["api_key_id"] == "api_key_1" ? openai.id : google.id;
    const auto by_connection = [](const auto& left, const auto& right) {
        return left.at("service") == right.at("service")
            ? left.at("api_key_id") < right.at("api_key_id") : left.at("service") < right.at("service");
    };
    std::sort(actual["choices"].begin(), actual["choices"].end(), by_connection);
    std::sort(expected["choices"].begin(), expected["choices"].end(), by_connection);
    EXPECT_EQ(actual, expected);
}

} // namespace
} // namespace cha::bridge
