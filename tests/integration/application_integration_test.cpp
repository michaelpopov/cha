#include "app/application.h"
#include "bridge/bridge_router.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

namespace cha {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

std::string answer_delta(bool responses, std::string_view text) {
    const Json delta = responses
        ? Json{{"type", "response.output_text.delta"}, {"delta", text}}
        : Json{{"choices", Json::array({Json{{"delta", {{"content", text}}}}})}};
    return "data: " + delta.dump() + "\n\n";
}

std::string unfinished_reply(bool responses, std::string_view text) {
    // Without Content-Length the transfer remains open until the server closes
    // the socket. No API completion event is sent.
    return "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
           "Connection: close\r\n\r\n" + answer_delta(responses, text);
}

std::string streamed_reply(
    bool responses, std::string_view text, std::string_view reasoning = {}) {
    std::string body;
    if (!reasoning.empty()) {
        const Json delta = responses
            ? Json{{"type", "response.reasoning_summary_text.delta"}, {"delta", reasoning}}
            : Json{{"choices", Json::array({Json{{"delta", {{"reasoning_content", reasoning}}}}})}};
        body = "data: " + delta.dump() + "\n\n";
    }
    body += answer_delta(responses, text);
    if (responses) {
        body += "data: " + Json{{"type", "response.completed"},
            {"response", {{"status", "completed"},
                          {"usage", {{"input_tokens", 1200},
                                     {"output_tokens", 300}}}}}}.dump() + "\n\n";
    } else {
        body += "data: " + Json{{"choices", Json::array()},
            {"usage", {{"prompt_tokens", 1200},
                       {"completion_tokens", 300}}}}.dump() + "\n\n";
        body += "data: [DONE]\n\n";
    }
    return http_response("text/event-stream", body);
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot read integration database");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Real bridge, application, provider transport and database, with only the
// upstream HTTP response supplied by a local server.
class ApplicationIntegration : public ::testing::Test {
protected:
    void initialize(int port, bool responses = false) {
        workspace_.write_provider("integration",
            "host = \"127.0.0.1\"\nport = " + std::to_string(port)
            + "\nhttps = false\nmode = \"net\"\nbase_path = \"/v1\"\n"
              "model = \"integration-model\"\nstream = true\n"
              "timeout_s = 5\nidle_timeout_s = 5\napi = \""
            + (responses ? "responses" : "chat_completions") + "\"\n");
        workspace_.write_character_config(
            "display_name = \"Guide\"\nprovider = \"integration\"\n");
        database_ = test::import_test_database(workspace_.root());
        config_ = workspace_.root() / "cha-config";
        std::filesystem::create_directories(config_);
        std::ofstream(config_ / "app.toml")
            << "vault = \"Integration\"\n"
               "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
        std::ofstream(config_ / "vault.toml")
            << "vault_name = \"Integration\"\ndata = "
            << std::quoted(database_.string()) << '\n';
        start();
    }

    void start(std::string password = {}) {
        const std::string argument = "--config=" + config_.string();
        const char* argv[] = {"itest", argument.c_str()};
        application_ = app::Application::open(
            parse_application_command(2, argv), std::move(password));
        router_ = std::make_unique<bridge::BridgeRouter>(*application_);
        connection_ = router_->open_connection();
        next_id_ = 1;
        epoch_ = 0;
        (void)call("app.bootstrap");
    }

    void stop() {
        if (router_) router_->shutdown();
        router_.reset();
        if (application_) {
            application_->request_shutdown();
            EXPECT_TRUE(application_->join_shutdown(5s));
            application_.reset();
        }
    }

    void TearDown() override { stop(); }

    Json call(std::string_view method, Json params = Json::object()) {
        const auto id = next_id_++;
        router_->handle_request(connection_, Json{
            {"connection_id", connection_}, {"id", id},
            {"context_epoch", epoch_}, {"method", method},
            {"params", std::move(params)}}.dump());
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            router_->run_tasks();
            router_->pump_output();
            if (auto batch = router_->take_delivery(connection_)) {
                router_->handle_ack(connection_, Json{
                    {"connection_id", connection_},
                    {"delivery_id", batch->at("delivery_id")}}.dump());
                for (const auto& message : batch->at("messages")) {
                    if (!message.contains("id") || message["id"] != id) continue;
                    if (!message.at("ok").get<bool>()) {
                        throw std::runtime_error(
                            std::string(method) + ": " + message.at("error").dump());
                    }
                    epoch_ = message.at("context_epoch").get<std::uint64_t>();
                    return message.at("result");
                }
            }
            router_->wait_for_work(5ms);
        }
        throw std::runtime_error(std::string(method) + " timed out");
    }

    Json create_session() {
        const auto created = call("session.create",
            {{"forum_id", "lobby"}, {"label", "Integration conversation"}});
        Json identity{{"forum_id", "lobby"}, {"session_id", created.at("id")}};
        (void)call("session.open", identity);
        return identity;
    }

    void submit(const Json& identity, std::string_view prompt) {
        Json params = identity;
        params["input"] = {{"text", prompt}};
        (void)call("session.submit", std::move(params));
    }

    Json wait_until_idle(const Json& identity) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            Json snapshot = call("session.snapshot", identity);
            if (!snapshot.at("generation").at("active").get<bool>()) return snapshot;
            router_->wait_for_work(5ms);
        }
        throw std::runtime_error("Integration generation did not finish");
    }

    Json chat(const Json& identity, std::string_view prompt) {
        submit(identity, prompt);
        return wait_until_idle(identity);
    }

    void wait_for_partial(const Json& identity, std::string_view text) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            const Json snapshot = call("session.snapshot", identity);
            const auto& transcript = snapshot.at("transcript");
            if (!transcript.empty() && transcript.back().at("text") == text
                && snapshot.at("generation").at("active").get<bool>()) return;
            router_->wait_for_work(5ms);
        }
        throw std::runtime_error("Did not observe an active partial reply");
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::filesystem::path config_;
    std::unique_ptr<app::Application> application_;
    std::unique_ptr<bridge::BridgeRouter> router_;
    std::string connection_;
    std::uint64_t next_id_{1};
    std::uint64_t epoch_{};
};

class ChatPersistenceIntegration : public ApplicationIntegration,
                                   public ::testing::WithParamInterface<bool> {};

TEST_P(ChatPersistenceIntegration, RestoresTranscriptUsageAndProviderHistoryAfterRestart) {
    const bool responses = GetParam();
    MockHttpServer server({streamed_reply(responses, "First answer"),
                           streamed_reply(responses, "Second answer")});
    server.start();
    initialize(server.port(), responses);
    const Json identity = create_session();
    const Json before = chat(identity, "First question");
    ASSERT_EQ(before.at("transcript").size(), 2U);
    const auto& answer = before.at("transcript").back();
    EXPECT_EQ(answer.at("text"), "First answer");
    EXPECT_EQ(answer.at("status"), "complete");
    EXPECT_EQ(answer.at("input_tokens"), 1200);
    EXPECT_EQ(answer.at("output_tokens"), 300);

    stop();
    start();
    (void)call("session.open", identity);
    EXPECT_EQ(call("session.snapshot", identity).at("transcript"), before.at("transcript"));
    const Json after = chat(identity, "Second question");
    ASSERT_EQ(after.at("transcript").size(), 4U);
    EXPECT_EQ(after.at("transcript").back().at("text"), "Second answer");
    server.join();
    ASSERT_EQ(server.requests().size(), 2U);
    const auto request = Json::parse(request_body(server.requests().back()));
    const auto& history = request.at(responses ? "input" : "messages");
    EXPECT_NE(history.dump().find("First question"), std::string::npos);
    EXPECT_NE(history.dump().find("Second question"), std::string::npos);
    bool found_answer = false;
    for (const auto& message : history) {
        if (message.at("role") == "assistant"
            && message.at("content").get<std::string>().ends_with("First answer")) {
            found_answer = true;
        }
    }
    EXPECT_TRUE(found_answer) << history.dump();
}

TEST_P(ChatPersistenceIntegration, UserStopPreservesPartialReplyButOmitsItFromNextRequest) {
    constexpr std::string_view partial = "PARTIAL_STOP_731";
    // Wait for client cancellation instead of sleeping for an assumed delay.
    MockHttpServer server({unfinished_reply(GetParam(), partial),
                           streamed_reply(GetParam(), "Recovered answer")}, true);
    server.start();
    initialize(server.port(), GetParam());
    const Json identity = create_session();
    submit(identity, "Stopped prompt");
    wait_for_partial(identity, partial);
    (void)call("session.stop", identity);
    const Json stopped = wait_until_idle(identity);
    ASSERT_EQ(stopped.at("transcript").size(), 2U);
    EXPECT_EQ(stopped.at("transcript").back().at("status"), "cancelled");
    EXPECT_EQ(stopped.at("transcript").back().at("text"), partial);
    stop();
    start();
    (void)call("session.open", identity);
    EXPECT_EQ(call("session.snapshot", identity).at("transcript"), stopped.at("transcript"));
    EXPECT_EQ(chat(identity, "Continue after stop").at("transcript").back().at("text"),
        "Recovered answer");
    server.join();
    ASSERT_EQ(server.requests().size(), 2U);
    const std::string next = request_body(server.requests().back());
    EXPECT_EQ(next.find(partial), std::string::npos) << next;
    EXPECT_NE(next.find("Continue after stop"), std::string::npos) << next;
}

TEST_P(ChatPersistenceIntegration, ShutdownPersistsPartialReplyAsCancelled) {
    constexpr std::string_view partial = "PARTIAL_SHUTDOWN_731";
    MockHttpServer server({unfinished_reply(GetParam(), partial),
                           streamed_reply(GetParam(), "Recovered answer")}, true);
    server.start();
    initialize(server.port(), GetParam());
    const Json identity = create_session();
    submit(identity, "Shutdown prompt");
    wait_for_partial(identity, partial);
    stop();
    start();
    (void)call("session.open", identity);
    const Json restored = call("session.snapshot", identity);
    ASSERT_EQ(restored.at("transcript").size(), 2U);
    EXPECT_EQ(restored.at("transcript").back().at("status"), "cancelled");
    EXPECT_EQ(restored.at("transcript").back().at("text"), partial);
    EXPECT_FALSE(restored.at("generation").at("active").get<bool>());
    EXPECT_EQ(chat(identity, "Continue after shutdown").at("transcript").back().at("text"),
        "Recovered answer");
    server.join();
    ASSERT_EQ(server.requests().size(), 2U);
    const std::string next = request_body(server.requests().back());
    EXPECT_EQ(next.find(partial), std::string::npos) << next;
    EXPECT_NE(next.find("Continue after shutdown"), std::string::npos) << next;
}

TEST_P(ChatPersistenceIntegration, TruncatedStreamStaysFailedAfterRestartAndIsNotReplayed) {
    constexpr std::string_view partial = "PARTIAL_TRUNCATED_731";
    MockHttpServer server({unfinished_reply(GetParam(), partial),
                           streamed_reply(GetParam(), "Recovered answer")});
    server.start();
    initialize(server.port(), GetParam());
    const Json identity = create_session();
    const Json failed = chat(identity, "Truncated prompt");
    ASSERT_EQ(failed.at("transcript").size(), 2U);
    EXPECT_EQ(failed.at("transcript").back().at("status"), "failed");
    EXPECT_EQ(failed.at("transcript").dump().find(partial), std::string::npos);
    stop();
    start();
    (void)call("session.open", identity);
    EXPECT_EQ(call("session.snapshot", identity).at("transcript"), failed.at("transcript"));
    EXPECT_EQ(chat(identity, "Continue after truncation").at("transcript").back().at("text"),
        "Recovered answer");
    server.join();
    ASSERT_EQ(server.requests().size(), 2U);
    const std::string next = request_body(server.requests().back());
    EXPECT_EQ(next.find(partial), std::string::npos) << next;
    EXPECT_EQ(next.find("Truncated prompt"), std::string::npos) << next;
    EXPECT_NE(next.find("Continue after truncation"), std::string::npos) << next;
}

TEST_P(ChatPersistenceIntegration, RepairsOpenTurnAfterProcessExitWithoutCleanup) {
    std::string partial = "PARTIAL_PROCESS_EXIT_731";
    MockHttpServer server({unfinished_reply(GetParam(), partial),
                           streamed_reply(GetParam(), "Recovered answer")}, true);
    server.start();
    initialize(server.port(), GetParam());
    const Json identity = create_session();
    stop();

    std::string executable = CHA_INTERRUPTED_APP_HELPER;
    std::string config = config_.string();
    std::string session = identity.at("session_id").get<std::string>();
    char* arguments[] = {executable.data(), config.data(), session.data(), partial.data(), nullptr};
    pid_t process{};
    ASSERT_EQ(posix_spawn(&process, executable.c_str(), nullptr, nullptr, arguments, environ), 0);
    int status{};
    pid_t waited{};
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    do {
        waited = waitpid(process, &status, WNOHANG);
        if (waited == process || (waited == -1 && errno != EINTR)) break;
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    if (waited != process) {
        (void)kill(process, SIGKILL);
        while (waitpid(process, &status, 0) == -1 && errno == EINTR) {}
        FAIL() << "Interrupted-application helper did not exit in time";
    }
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0) << "Helper must exit only after observing partial text";

    start();
    (void)call("session.open", identity);
    const Json repaired = call("session.snapshot", identity);
    ASSERT_EQ(repaired.at("transcript").size(), 2U);
    const auto& error = repaired.at("transcript").back();
    EXPECT_EQ(error.at("status"), "failed");
    EXPECT_EQ(error.at("text"), "Response interrupted before it finished");
    EXPECT_FALSE(repaired.at("generation").at("active").get<bool>());
    // Repair is durable and must not add another error on the next open.
    stop();
    start();
    (void)call("session.open", identity);
    EXPECT_EQ(call("session.snapshot", identity).at("transcript"), repaired.at("transcript"));
    EXPECT_EQ(chat(identity, "Continue after interruption").at("transcript").back().at("text"),
        "Recovered answer");
    server.join();
    ASSERT_EQ(server.requests().size(), 2U);
    const std::string next = request_body(server.requests().back());
    EXPECT_EQ(next.find(partial), std::string::npos) << next;
    EXPECT_EQ(next.find("Interrupted prompt"), std::string::npos) << next;
    EXPECT_NE(next.find("Continue after interruption"), std::string::npos) << next;
}

TEST_P(ChatPersistenceIntegration, ExcludesReasoningFromTranscriptAndContextAfterRestart) {
    constexpr std::string_view reasoning = "PRIVATE_REASONING_731";
    MockHttpServer server({streamed_reply(GetParam(), "Visible answer", reasoning),
                           streamed_reply(GetParam(), "Next answer")});
    server.start();
    initialize(server.port(), GetParam());
    const Json identity = create_session();
    const Json before = chat(identity, "First question");
    ASSERT_EQ(before.at("transcript").size(), 2U);
    EXPECT_EQ(before.at("transcript").back().at("text"), "Visible answer");
    EXPECT_EQ(before.dump().find(reasoning), std::string::npos);
    stop();
    start();
    (void)call("session.open", identity);
    EXPECT_EQ(call("session.snapshot", identity).at("transcript"), before.at("transcript"));
    const Json after = chat(identity, "Next question");
    EXPECT_EQ(after.at("transcript").back().at("text"), "Next answer");
    EXPECT_EQ(after.dump().find(reasoning), std::string::npos);
    server.join();
    ASSERT_EQ(server.requests().size(), 2U);
    const std::string next = request_body(server.requests().back());
    EXPECT_EQ(next.find(reasoning), std::string::npos) << next;
    EXPECT_NE(next.find("Visible answer"), std::string::npos) << next;
}

TEST_F(ApplicationIntegration, ProtectedVaultRejectsWrongPasswordAndContinuesAfterRestart) {
    MockHttpServer server({streamed_reply(false, "Before protection"),
                           streamed_reply(false, "After protection")});
    server.start();
    initialize(server.port());
    const Json identity = create_session();
    const Json before = chat(identity, "Remember this");
    ASSERT_EQ(before.at("transcript").size(), 2U);
    (void)call("vault.update", {{"vault_name", "Integration"},
        {"display_name", "Integration"}, {"password", "integration-password"}});
    stop();
    const std::string encrypted = file_bytes(database_);
    EXPECT_FALSE(encrypted.starts_with("SQLite format 3"));
    EXPECT_THROW(start(), VaultPasswordError);
    EXPECT_THROW(start("wrong-password"), VaultPasswordError);
    EXPECT_EQ(file_bytes(database_), encrypted);
    start("integration-password");
    (void)call("session.open", identity);
    EXPECT_EQ(call("session.snapshot", identity).at("transcript"), before.at("transcript"));
    const Json after = chat(identity, "Continue");
    ASSERT_EQ(after.at("transcript").size(), 4U);
    EXPECT_EQ(after.at("transcript").back().at("text"), "After protection");
    server.join();
}

TEST_F(ApplicationIntegration, ProviderFailurePersistsAndNextTurnSucceeds) {
    MockHttpServer server({
        "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n",
        streamed_reply(false, "Recovered answer")});
    server.start();
    initialize(server.port());
    const Json identity = create_session();
    const Json failed = chat(identity, "First attempt");
    EXPECT_FALSE(failed.at("generation").at("active").get<bool>());
    ASSERT_EQ(failed.at("transcript").size(), 2U);
    EXPECT_EQ(failed.at("transcript").back().at("status"), "failed");
    stop();
    start();
    (void)call("session.open", identity);
    EXPECT_EQ(call("session.snapshot", identity).at("transcript"), failed.at("transcript"));
    const Json recovered = chat(identity, "Try again");
    EXPECT_EQ(recovered.at("transcript").back().at("text"), "Recovered answer");
    EXPECT_EQ(recovered.at("transcript").back().at("status"), "complete");
    server.join();
    ASSERT_EQ(server.requests().size(), 2U);
    const std::string next = request_body(server.requests().back());
    EXPECT_EQ(next.find("First attempt"), std::string::npos) << next;
    EXPECT_NE(next.find("Try again"), std::string::npos) << next;
}

INSTANTIATE_TEST_SUITE_P(ProviderApis, ChatPersistenceIntegration,
    ::testing::Values(false, true), [](const auto& info) {
        return info.param ? "Responses" : "ChatCompletions";
    });

} // namespace
} // namespace cha
