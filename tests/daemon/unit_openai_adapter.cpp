#include "app/application.h"
#include "daemon/openai_adapter.h"
#include "daemon/scgi.h"
#include "support/test_workspace.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace cha::daemon {
namespace {

using app::Application;

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
    const ConfigurationDirectory loaded =
        load_configuration_directory(config_directory);
    const VaultDefinition* const vault =
        find_vault(loaded.vaults, loaded.startup_vault);
    return {
        .config_directory = loaded.directory,
        .vaults = loaded.vaults,
        .vault = *vault,
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

nlohmann::json chat_body(
    std::string_view model,
    const nlohmann::json& messages,
    nlohmann::json extra = nlohmann::json::object()) {
    extra["model"] = model;
    extra["messages"] = messages;
    return extra;
}

struct CgiResponse {
    int status{};
    nlohmann::json json;
};

CgiResponse exchange(
    Application& application, const ScgiRequest& request) {
    int fds[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::runtime_error("socketpair failed");
    }
    UniqueFd server(fds[0]);
    UniqueFd client(fds[1]);
    std::atomic<bool> stop{false};
    handle_request(application, request, server.get(), stop);
    server.close();
    std::string raw;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::recv(client.get(), buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        raw.append(buffer, static_cast<std::size_t>(count));
    }
    CgiResponse response;
    const auto prefix = raw.find("Status: ");
    if (prefix == 0) {
        response.status = std::stoi(raw.substr(8));
    }
    const auto body_at = raw.find("\r\n\r\n");
    if (body_at != std::string::npos) {
        response.json = nlohmann::json::parse(raw.substr(body_at + 4));
    }
    return response;
}

class OpenAiAdapterTest : public testing::Test {
protected:
    void SetUp() override {
        workspace_.add_forum("stoics", "Stoics", "guide");
        database_ = test::import_test_database(workspace_.root());
        application_ = Application::open(make_command(workspace_, database_));
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::unique_ptr<Application> application_;
};

TEST_F(OpenAiAdapterTest, ListsForumsAndOmitsEntrance) {
    const nlohmann::json listed = models_list(*application_);
    EXPECT_EQ(listed.at("object"), "list");
    ASSERT_TRUE(listed.at("data").is_array());
    std::vector<std::string> ids;
    for (const auto& model : listed.at("data")) {
        EXPECT_EQ(model.at("object"), "model");
        EXPECT_EQ(model.at("owned_by"), "cha");
        EXPECT_EQ(model.at("created"), 0);
        ids.push_back(model.at("id").get<std::string>());
    }
    EXPECT_NE(std::find(ids.begin(), ids.end(), "The Lobby"), ids.end());
    EXPECT_NE(std::find(ids.begin(), ids.end(), "Stoics"), ids.end());
    EXPECT_EQ(
        std::find(ids.begin(), ids.end(), std::string(entrance_id)),
        ids.end());

    const CgiResponse response = exchange(
        *application_,
        {.method = "GET", .document_uri = "/v1/models", .body = {}});
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.json, listed);
}

TEST_F(OpenAiAdapterTest, ReturnsOpenAiErrorEnvelopes) {
    const CgiResponse missing = exchange(
        *application_,
        {.method = "GET", .document_uri = "/v1/unknown", .body = {}});
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.json.at("error").at("message"), "Unknown endpoint");
    EXPECT_EQ(missing.json.at("error").at("type"), "invalid_request_error");
    EXPECT_EQ(missing.json.at("error").at("code"), "not_found");

    const CgiResponse method = exchange(
        *application_,
        {.method = "POST", .document_uri = "/v1/models", .body = {}});
    EXPECT_EQ(method.status, 404);
}

TEST_F(OpenAiAdapterTest, AcceptsContentArraysAndIgnoresUnusedOptions) {
    const auto parsed = parse_chat_request(
        chat_body(
            "The Lobby",
            nlohmann::json::array({
                {{"role", "system"}, {"content", "ignore"}},
                {{"role", "user"},
                 {"content",
                  nlohmann::json::array(
                      {{{"type", "image_url"}, {"image_url", {{"url", "x"}}}},
                       {{"type", "text"}, {"text", "hel"}},
                       {{"type", "text"}, {"text", "lo"}}})}},
            }),
            {{"temperature", 0.2},
             {"max_tokens", 16},
             {"tools", nlohmann::json::array()},
             {"tool_choice", "auto"}})
            .dump(),
        *application_);
    const auto* request = std::get_if<ParsedChatRequest>(&parsed);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->model, "lobby");
    EXPECT_EQ(request->model_name, "The Lobby");
    EXPECT_EQ(request->user_text, "hello");
    EXPECT_FALSE(request->stream);
    EXPECT_FALSE(request->tag);
}

TEST_F(OpenAiAdapterTest, AcceptsPreviousForumIds) {
    const auto parsed = parse_chat_request(
        chat_body("lobby", nlohmann::json::array({
            {{"role", "user"}, {"content", "hello"}},
        })).dump(),
        *application_);
    const auto* request = std::get_if<ParsedChatRequest>(&parsed);
    ASSERT_NE(request, nullptr);
    EXPECT_EQ(request->model, "lobby");
    EXPECT_EQ(request->model_name, "lobby");
}

TEST_F(OpenAiAdapterTest, RejectsPromptSizeBoundaryAndUnknownModels) {
    const std::size_t limit = application_->settings().prompt_limit;
    const auto allowed = parse_chat_request(
        chat_body(
            "The Lobby",
            nlohmann::json::array({
                {{"role", "user"}, {"content", std::string(limit, 'a')}},
            }))
            .dump(),
        *application_);
    EXPECT_TRUE(std::holds_alternative<ParsedChatRequest>(allowed));

    const auto too_large = parse_chat_request(
        chat_body(
            "The Lobby",
            nlohmann::json::array({
                {{"role", "user"},
                 {"content", std::string(limit + 1, 'a')}},
            }))
            .dump(),
        *application_);
    const auto* error = std::get_if<ApiError>(&too_large);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->status, 400);

    const auto entrance = parse_chat_request(
        chat_body(
            std::string(entrance_id),
            nlohmann::json::array(
                {{{"role", "user"}, {"content", "hello"}}}))
            .dump(),
        *application_);
    const auto* hidden = std::get_if<ApiError>(&entrance);
    ASSERT_NE(hidden, nullptr);
    EXPECT_EQ(hidden->status, 404);

    const auto unknown = parse_chat_request(
        chat_body(
            "missing",
            nlohmann::json::array(
                {{{"role", "user"}, {"content", "hello"}}}))
            .dump(),
        *application_);
    const auto* absent = std::get_if<ApiError>(&unknown);
    ASSERT_NE(absent, nullptr);
    EXPECT_EQ(absent->status, 404);
}

TEST_F(OpenAiAdapterTest, SelectsTagsAndReportsForumMismatch) {
    const auto continued = parse_chat_request(
        chat_body(
            "The Lobby",
            nlohmann::json::array({
                {{"role", "user"}, {"content", "hi"}},
                {{"role", "assistant"},
                 {"content", "[//]: # (cha lobby/old)\nfirst"}},
                {{"role", "assistant"}, {"content", "untagged"}},
                {{"role", "user"}, {"content", "again"}},
            }))
            .dump(),
        *application_);
    const auto* tagged = std::get_if<ParsedChatRequest>(&continued);
    ASSERT_NE(tagged, nullptr);
    EXPECT_EQ(tagged->model, "lobby");
    EXPECT_EQ(tagged->model_name, "The Lobby");
    ASSERT_TRUE(tagged->tag);
    EXPECT_EQ(tagged->tag->session_id, "old");

    const auto mismatch = parse_chat_request(
        chat_body(
            "The Lobby",
            nlohmann::json::array({
                {{"role", "assistant"},
                 {"content", "[//]: # (cha lobby/old)"}},
                {{"role", "assistant"},
                 {"content", "[//]: # (cha stoics/new)"}},
                {{"role", "user"}, {"content", "again"}},
            }))
            .dump(),
        *application_);
    const auto* wrong = std::get_if<ApiError>(&mismatch);
    ASSERT_NE(wrong, nullptr);
    EXPECT_EQ(wrong->status, 400);

    const auto missing = parse_chat_request(
        chat_body(
            "The Lobby",
            nlohmann::json::array({
                {{"role", "assistant"}, {"content", "no tag"}},
                {{"role", "user"}, {"content", "hello"}},
            }))
            .dump(),
        *application_);
    const auto* absent = std::get_if<ApiError>(&missing);
    ASSERT_NE(absent, nullptr);
    EXPECT_EQ(absent->status, 400);
    EXPECT_EQ(
        absent->message, "This chat has no CHA session. Start a new chat.");
}

TEST_F(OpenAiAdapterTest, InvalidChatDoesNotCreateASession) {
    const auto epoch = application_->context_epoch();
    const CgiResponse invalid = exchange(
        *application_,
        {.method = "POST",
         .document_uri = "/v1/chat/completions",
         .body = chat_body(
                     "The Lobby",
                     nlohmann::json::array({
                         {{"role", "assistant"}, {"content", "no tag"}},
                         {{"role", "user"}, {"content", "hello"}},
                     }))
                     .dump()});
    EXPECT_EQ(invalid.status, 400);
    EXPECT_TRUE(application_->list_sessions("lobby", epoch).empty());
}

} // namespace
} // namespace cha::daemon
