#include "app/application.h"
#include "bridge/bridge_router.h"
#include "storage/session_lease.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "runtime/command_queue.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
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
    ASSERT_EQ(server.requests().size(), 3U);
    EXPECT_TRUE(server.requests().back().starts_with("HEAD "));
}

TEST_F(BridgeRouterTest, MaintenanceUploadConflictExplainsHowToRetry) {
    bootstrap_epoch();
    MockHttpServer server({
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
    ASSERT_EQ(server.requests().size(), 1U);
    EXPECT_NE(server.requests()[0].find("If-Match: \"previous-etag\""),
        std::string::npos);
}

TEST_F(BridgeRouterTest, MaintenanceFilesystemFailureHidesPrivatePath) {
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
    ASSERT_FALSE(reply["ok"]);
    EXPECT_EQ(reply["error"]["code"], "internal_error");
    EXPECT_EQ(reply["error"]["message"], "The request could not be completed.");
    EXPECT_EQ(reply["error"]["message"].get<std::string>().find(
        workspace_.root().string()), std::string::npos);
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
    const std::string message = failed["error"]["message"];
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

} // namespace
} // namespace cha::bridge
