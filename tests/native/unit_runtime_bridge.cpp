#include "runtime_bridge.h"

#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/path_name.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;

#ifdef __APPLE__
TEST(NativeRuntime, SupportsSecureWebSockets) {
    EXPECT_EQ(cha_runtime_supports_secure_websockets(), 1);
}
#endif

struct CapturedDeliveries {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<nlohmann::json> batches;
};

void on_delivery(void* context, const char*, const char* json) {
    auto* captured = static_cast<CapturedDeliveries*>(context);
    std::lock_guard lock(captured->mutex);
    captured->batches.push_back(nlohmann::json::parse(json));
    captured->ready.notify_all();
}

std::filesystem::path make_config(const cha::test::TestWorkspace& workspace) {
    const auto database = cha::test::import_test_database(workspace.root());
    const auto config = workspace.root() / "cha-config";
    std::filesystem::create_directories(config);
    std::ofstream(config / "app.toml")
        << "vault = \"Test\"\n[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    std::ofstream(config / "test.toml")
        << "vault_name = \"Test\"\ndata = " << std::quoted(cha::utf8_path(database))
        << "\n";
    return config;
}

nlohmann::json wait_batch(
    CapturedDeliveries& captured,
    std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock(captured.mutex);
    if (captured.ready.wait_for(lock, timeout, [&] {
            return !captured.batches.empty();
        })) {
        nlohmann::json batch = std::move(captured.batches.front());
        captured.batches.erase(captured.batches.begin());
        return batch;
    }
    return {};
}

nlohmann::json request(
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

class NativeRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        config_ = make_config(workspace_);
        resources_ = workspace_.root();
        captured_ = std::make_unique<CapturedDeliveries>();
        char* error = nullptr;
        int32_t password_error = 0;
        runtime_ = cha_runtime_create(
            cha::utf8_path(config_).c_str(),
            cha::utf8_path(resources_).c_str(),
            "",
            &password_error,
            &error);
        ASSERT_NE(runtime_, nullptr) << (error ? error : "");
        cha_string_free(error);
        cha_runtime_set_delivery_callback(runtime_, on_delivery, captured_.get());
        char* connection_error = nullptr;
        char* connection = cha_runtime_open_connection(
            runtime_, &connection_error);
        ASSERT_NE(connection, nullptr) << (connection_error ? connection_error : "");
        connection_ = connection;
        cha_string_free(connection);
        cha_string_free(connection_error);
    }

    void TearDown() override {
        if (runtime_) {
            if (!connection_.empty()) {
                cha_runtime_close_connection(runtime_, connection_.c_str());
            }
            cha_runtime_request_shutdown(runtime_);
            (void)cha_runtime_join_shutdown(runtime_, 2000);
            cha_runtime_destroy(runtime_);
            runtime_ = nullptr;
        }
    }

    nlohmann::json call(
        std::string_view method,
        nlohmann::json params = nlohmann::json::object(),
        std::uint64_t epoch = 0) {
        const auto id = next_id_++;
        const auto body = request(
            connection_, id, epoch == 0 ? epoch_ : epoch, method, std::move(params));
        cha_runtime_handle_message(
            runtime_, connection_.c_str(), body.dump().c_str());
        nlohmann::json reply;
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (reply.empty() && std::chrono::steady_clock::now() < deadline) {
            auto batch = wait_batch(*captured_, 100ms);
            if (batch.is_null() || batch.empty()) continue;
            for (const auto& message : batch.at("messages")) {
                if (message.contains("id") && message["id"] == id) reply = message;
            }
            const auto ack = nlohmann::json{
                {"connection_id", connection_},
                {"delivery_id", batch.at("delivery_id")},
            };
            cha_runtime_handle_message(
                runtime_, connection_.c_str(), ack.dump().c_str());
        }
        EXPECT_FALSE(reply.empty()) << method;
        return reply;
    }

    cha::test::TestWorkspace workspace_;
    std::filesystem::path config_;
    std::filesystem::path resources_;
    std::unique_ptr<CapturedDeliveries> captured_;
    ChaRuntime* runtime_{};
    std::string connection_;
    std::uint64_t epoch_{0};
    std::uint64_t next_id_{1};
};

#if GTEST_HAS_DEATH_TEST
TEST(NativeRuntimeDeathTest, ShutdownDeadlineIncludesBlockedDelivery) {
    // A blocked host callback must not turn grace_ms into an unbounded join.
    // Deliberately leave the worker blocked; production exits the process on
    // this path, retaining its dependencies rather than detaching the thread.
    EXPECT_EXIT(([] {
        cha::test::TestWorkspace workspace;
        const auto config = make_config(workspace);
        char* error = nullptr;
        int32_t password_error{};
        auto* runtime = cha_runtime_create(
            cha::utf8_path(config).c_str(), cha::utf8_path(workspace.root()).c_str(),
            "", &password_error, &error);
        if (!runtime) std::_Exit(2);
        struct Blocked {
            std::mutex mutex;
            std::condition_variable changed;
            bool entered{};
        } blocked;
        cha_runtime_set_delivery_callback(runtime, [](void* context, const char*, const char*) {
            auto& blocked = *static_cast<Blocked*>(context);
            std::unique_lock lock(blocked.mutex);
            blocked.entered = true;
            blocked.changed.notify_all();
            blocked.changed.wait(lock, [] { return false; });
        }, &blocked);
        char* connection = cha_runtime_open_connection(runtime, &error);
        if (!connection) std::_Exit(3);
        const auto message = request(connection, 1, 0, "bridge.info").dump();
        cha_runtime_handle_message(runtime, connection, message.c_str());
        {
            std::unique_lock lock(blocked.mutex);
            if (!blocked.changed.wait_for(lock, 2s, [&] { return blocked.entered; })) std::_Exit(4);
        }
        cha_runtime_request_shutdown(runtime);
        const auto start = std::chrono::steady_clock::now();
        const auto joined = cha_runtime_join_shutdown(runtime, 20);
        std::_Exit(joined == 0 && std::chrono::steady_clock::now() - start < 500ms ? 0 : 1);
    }()), ::testing::ExitedWithCode(0), "");
}
#endif

TEST_F(NativeRuntimeTest, RejectsUnknownAndUnownedMediaResources) {
    char* mime = nullptr;
    void* bytes = nullptr;
    uint64_t size = 0;
    char* error = nullptr;
    EXPECT_EQ(
        cha_runtime_read_resource(
            runtime_, connection_.c_str(), "../secret",
            &mime, &bytes, &size, &error),
        0);
    EXPECT_EQ(size, 0U);
    cha_string_free(error);
    cha_string_free(mime);
    cha_bytes_free(bytes);

    mime = nullptr;
    bytes = nullptr;
    size = 0;
    error = nullptr;
    EXPECT_EQ(
        cha_runtime_read_resource(
            runtime_, connection_.c_str(), "r1",
            &mime, &bytes, &size, &error),
        0);
    cha_string_free(error);
    cha_string_free(mime);
    cha_bytes_free(bytes);
}

TEST_F(NativeRuntimeTest, StartsWithoutAListenerAndRunsFirstFlow) {
    auto info = call("bridge.info");
    ASSERT_TRUE(info["ok"]);
    EXPECT_EQ(info["result"]["protocol_version"], 1);
    EXPECT_TRUE(
        info["result"]["platform"] == "macos"
        || info["result"]["platform"] == "windows"
        || info["result"]["platform"] == "unknown");

    auto bootstrap = call("app.bootstrap");
    ASSERT_TRUE(bootstrap["ok"]);
    epoch_ = bootstrap["result"]["context_epoch"].get<std::uint64_t>();
    EXPECT_GE(epoch_, 1U);

    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Native"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    auto opened = call(
        "session.open",
        {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(opened["ok"]);
    auto submitted = call(
        "session.submit",
        {{"forum_id", "lobby"},
         {"session_id", session_id},
         {"input", {{"text", "Hello"}}}});
    ASSERT_TRUE(submitted["ok"]);
    EXPECT_TRUE(submitted["result"]["clear_input"].get<bool>());
    auto stopped = call(
        "session.stop", {{"forum_id", "lobby"}, {"session_id", session_id}});
    EXPECT_TRUE(stopped["ok"]);
    auto snapshot = call(
        "session.snapshot",
        {{"forum_id", "lobby"}, {"session_id", session_id}});
    ASSERT_TRUE(snapshot["ok"]);
    EXPECT_EQ(snapshot["result"]["session_id"], session_id);
}

TEST_F(NativeRuntimeTest, StopAndShutdownProgressDuringAStalledR2Download) {
    cha::MockHttpServer server({"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n"}, true);
    epoch_ = call("app.bootstrap")["result"]["context_epoch"];
    const auto created = call("session.create", {{"forum_id", "lobby"}, {"label", "Stop"}});
    const auto identity = nlohmann::json{{"forum_id", "lobby"},
        {"session_id", created["result"]["id"]}};
    ASSERT_TRUE(call("session.open", identity)["ok"]);
    ASSERT_TRUE(call("r2Storage.save", {
        {"display_name", "Backups"},
        {"url", "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket"},
        {"access_key_id", "access"}, {"secret_key", "secret"}})["ok"]);
    server.start();
    const auto download = request(connection_, next_id_++, epoch_,
        "vault.r2.download", {{"name", "remote"}});
    cha_runtime_handle_message(runtime_, connection_.c_str(), download.dump().c_str());
    const bool started = server.wait_for_requests(1, 2s);
    EXPECT_TRUE(started);
    if (started) {
        const auto start = std::chrono::steady_clock::now();
        const auto stopped = call("session.stop", identity);
        EXPECT_TRUE(stopped.value("ok", false)) << stopped.dump();
        EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
        const auto closed = call("session.close", identity);
        EXPECT_TRUE(closed.value("ok", false)) << closed.dump();
    }
    cha_runtime_request_shutdown(runtime_);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(cha_runtime_join_shutdown(runtime_, 1000), 1);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 1500ms);
    server.join();
}

TEST_F(NativeRuntimeTest, CloseAndHandleDoNotPropagateExceptions) {
    cha_runtime_handle_message(runtime_, connection_.c_str(), "not-json");
    cha_runtime_handle_message(runtime_, connection_.c_str(), "[]");
    cha_runtime_handle_message(runtime_, connection_.c_str(), "{}");
    cha_runtime_close_connection(runtime_, "missing-connection");
    auto info = call("bridge.info");
    ASSERT_TRUE(info["ok"]);
}

TEST_F(NativeRuntimeTest, VaultSwitchPublishesANewEpochAndRejectsStaleWork) {
    auto bootstrap = call("app.bootstrap");
    ASSERT_TRUE(bootstrap["ok"]);
    epoch_ = bootstrap["result"]["context_epoch"].get<std::uint64_t>();
    auto listed = call("vault.list");
    ASSERT_TRUE(listed["ok"]);
    ASSERT_TRUE(listed["result"].is_array());
    EXPECT_GE(listed["result"].size(), 1U);

    auto created = call(
        "vault.create",
        {{"display_name", "Extra"},
         {"copy_from", nullptr},
         {"password", nullptr}});
    ASSERT_TRUE(created["ok"]) << created.dump();

    const auto old_epoch = epoch_;
    auto switched = call(
        "vault.switch", {{"vault_name", "Extra"}, {"password", nullptr}});
    ASSERT_TRUE(switched["ok"]) << switched.dump();
    epoch_ = switched["result"]["context_epoch"].get<std::uint64_t>();
    EXPECT_GT(epoch_, old_epoch);

    auto rejected = call(
        "session.create",
        {{"forum_id", "lobby"}, {"label", "Stale"}},
        old_epoch);
    ASSERT_TRUE(rejected.contains("ok"));
    EXPECT_FALSE(rejected["ok"]);
    EXPECT_EQ(rejected["error"]["code"], "vault_changed");
}

TEST_F(NativeRuntimeTest, VaultSwitchPersistsAcrossRestart) {
    auto bootstrap = call("app.bootstrap");
    ASSERT_TRUE(bootstrap["ok"]);
    epoch_ = bootstrap["result"]["context_epoch"].get<std::uint64_t>();
    auto created = call(
        "vault.create",
        {{"display_name", "Kept"},
         {"copy_from", nullptr},
         {"password", nullptr}});
    ASSERT_TRUE(created["ok"]) << created.dump();
    auto switched = call(
        "vault.switch", {{"vault_name", "Kept"}, {"password", nullptr}});
    ASSERT_TRUE(switched["ok"]) << switched.dump();

    cha_runtime_close_connection(runtime_, connection_.c_str());
    cha_runtime_request_shutdown(runtime_);
    ASSERT_EQ(cha_runtime_join_shutdown(runtime_, 2000), 1);
    EXPECT_EQ(cha_runtime_join_shutdown(runtime_, 2000), 1);
    cha_runtime_destroy(runtime_);
    runtime_ = nullptr;
    connection_.clear();

    char* error = nullptr;
    int32_t password_error = 0;
    runtime_ = cha_runtime_create(
        cha::utf8_path(config_).c_str(),
        cha::utf8_path(resources_).c_str(),
        "",
        &password_error,
        &error);
    ASSERT_NE(runtime_, nullptr) << (error ? error : "");
    cha_string_free(error);
    cha_runtime_set_delivery_callback(runtime_, on_delivery, captured_.get());
    char* connection = cha_runtime_open_connection(runtime_, &error);
    ASSERT_NE(connection, nullptr);
    connection_ = connection;
    cha_string_free(connection);
    cha_string_free(error);
    next_id_ = 1;
    epoch_ = 0;
    auto restarted = call("app.bootstrap");
    ASSERT_TRUE(restarted["ok"]) << restarted.dump();
    EXPECT_EQ(restarted["result"]["bootstrap"]["vault_name"], "Kept");
}

TEST_F(NativeRuntimeTest, SaveFileUsesTemporaryReplaceAndHonorsEpoch) {
    auto bootstrap = call("app.bootstrap");
    ASSERT_TRUE(bootstrap["ok"]);
    epoch_ = bootstrap["result"]["context_epoch"].get<std::uint64_t>();
    const auto destination = workspace_.root() / "export.md";
    std::ofstream(destination) << "original";
    const std::string payload = "# Exported\n";
    char* error = nullptr;
    EXPECT_EQ(
        cha_runtime_save_file(
            runtime_,
            epoch_,
            cha::utf8_path(destination).c_str(),
            payload.c_str(),
            payload.size(),
            &error),
        1);
    cha_string_free(error);
    std::ifstream input(destination);
    std::string body(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(body, payload);

    const auto empty_destination = workspace_.root() / "empty.txt";
    EXPECT_EQ(
        cha_runtime_save_file(
            runtime_,
            epoch_,
            cha::utf8_path(empty_destination).c_str(),
            nullptr,
            0,
            &error),
        1);
    cha_string_free(error);
    std::ifstream empty_input(empty_destination);
    std::string empty_body(
        (std::istreambuf_iterator<char>(empty_input)),
        std::istreambuf_iterator<char>());
    EXPECT_TRUE(empty_body.empty());

    EXPECT_EQ(
        cha_runtime_save_file(
            runtime_,
            epoch_ + 99,
            cha::utf8_path(destination).c_str(),
            "stale",
            5,
            &error),
        0);
    ASSERT_NE(error, nullptr);
    EXPECT_STREQ(error, "The vault context has changed.");
    cha_string_free(error);
    std::ifstream after(destination);
    std::string kept(
        (std::istreambuf_iterator<char>(after)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(kept, payload);
}

TEST_F(NativeRuntimeTest, ExportSessionWritesMarkdownThroughSaveFile) {
    auto bootstrap = call("app.bootstrap");
    ASSERT_TRUE(bootstrap["ok"]);
    epoch_ = bootstrap["result"]["context_epoch"].get<std::uint64_t>();
    auto created = call(
        "session.create", {{"forum_id", "lobby"}, {"label", "Exported"}});
    ASSERT_TRUE(created["ok"]);
    const std::string session_id = created["result"]["id"];
    const auto destination = workspace_.root() / "session.md";
    char* error = nullptr;
    EXPECT_EQ(
        cha_runtime_export_session(
            runtime_,
            epoch_,
            "lobby",
            session_id.c_str(),
            cha::utf8_path(destination).c_str(),
            &error),
        1);
    cha_string_free(error);
    std::ifstream input(destination);
    std::string body(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(body.find("CHA session: Exported"), std::string::npos);
}

TEST_F(NativeRuntimeTest, CloseAndReopenConnectionDoesNotCrossResolve) {
    auto bootstrap = call("app.bootstrap");
    ASSERT_TRUE(bootstrap["ok"]);
    epoch_ = bootstrap["result"]["context_epoch"].get<std::uint64_t>();
    cha_runtime_close_connection(runtime_, connection_.c_str());
    char* error = nullptr;
    char* next = cha_runtime_open_connection(runtime_, &error);
    ASSERT_NE(next, nullptr);
    connection_ = next;
    cha_string_free(next);
    cha_string_free(error);
    next_id_ = 1;
    auto info = call("bridge.info");
    ASSERT_TRUE(info["ok"]);
}

} // namespace
