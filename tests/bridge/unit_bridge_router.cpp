#include "app/application.h"
#include "bridge/bridge_router.h"
#include "support/test_workspace.h"
#include "web/command_queue.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <string>
#include <thread>

namespace cha::bridge {
namespace {

using namespace std::chrono_literals;
using cha::web::ApplicationCommand;
using cha::web::ConfigurationDirectory;
using cha::web::ConfigurationTransport;
using cha::web::VaultDefinition;
using cha::web::find_vault;
using cha::web::load_configuration_directory;

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
        config_directory, ConfigurationTransport::native);
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
        application_ = cha::app::Application::open(make_command(workspace_, database_));
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
    std::unique_ptr<cha::app::Application> application_;
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

TEST_F(BridgeRouterTest, RejectsMalformedUnknownStaleDuplicateAndUnavailableMethods) {
    bootstrap_epoch();
    const auto before = application_->live_sessions().snapshot().live_session_count;

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

    EXPECT_EQ(
        application_->live_sessions().snapshot().live_session_count, before);
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
    cha::web::CommandReply reply;
    bool called = false;
    reply.set_ready_callback([&] { called = true; });
    reply.abandon();
    EXPECT_FALSE(reply.complete(cha::web::CommandResult{.clear_input = true}));
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
    for (const auto& message : batch->at("messages")) {
        if (message.contains("error")
            && message["error"]["message"] == "Too many in-flight requests.") {
            saw_limit = true;
        }
    }
    EXPECT_TRUE(saw_limit);
    router_->handle_request(
        connection_,
        request_json(connection_, 3, 0, "bridge.info").dump());
    EXPECT_FALSE(wait_delivery(*router_, connection_, 50ms));
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

} // namespace
} // namespace cha::bridge
