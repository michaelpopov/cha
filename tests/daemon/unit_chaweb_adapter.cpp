#include "app/application.h"
#include "daemon/chaweb_adapter.h"
#include "daemon/scgi.h"
#include "runtime/runtime_settings.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace cha::daemon {
namespace {

using app::Application;
using namespace std::chrono_literals;

constexpr std::string_view json_type = "application/json";
constexpr std::string_view bootstrap_path = "/api/cha/v1/bootstrap";

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

void disable_naming(const test::TestWorkspace& workspace) {
    const auto directory = workspace.root() / "system/session";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml")
        << "naming_provider = \"absent\"\n";
}

void use_net_provider(
    const test::TestWorkspace& workspace, int port) {
    workspace.write_provider(
        "remote",
        "host = \"127.0.0.1\"\nport = " + std::to_string(port)
            + "\nhttps = false\nmode = \"net\"\nmodel = \"fake\"\n"
              "api = \"chat_completions\"\nstream = false\ntimeout_s = 20\n");
    workspace.write_character_config(
        "display_name = \"Guide\"\nprovider = \"remote\"\n");
}

std::string http_json(std::string_view body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
        + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n" + std::string(body);
}

std::string sessions_path(std::string_view forum) {
    return "/api/cha/v1/forums/" + std::string(forum) + "/sessions";
}

std::string session_path(std::string_view forum, std::string_view id) {
    return sessions_path(forum) + "/" + std::string(id);
}

nlohmann::json text_body(std::string_view text) {
    return {{"text", text}};
}

struct CgiResponse {
    int status{};
    std::string raw;
    nlohmann::json json;
};

CgiResponse exchange(
    Application& application,
    const ScgiRequest& request,
    ChaWebDeleteSession delete_session = nullptr) {
    int fds[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::runtime_error("socketpair failed");
    }
    UniqueFd server(fds[0]);
    UniqueFd client(fds[1]);
    std::atomic<bool> stop{false};
    handle_chaweb_request(
        application, request, server.get(), stop, delete_session);
    server.close();
    CgiResponse response;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::recv(client.get(), buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        response.raw.append(buffer, static_cast<std::size_t>(count));
    }
    if (response.raw.starts_with("Status: ")) {
        response.status = std::stoi(response.raw.substr(8));
    }
    const auto body_at = response.raw.find("\r\n\r\n");
    if (body_at != std::string::npos) {
        const std::string body = response.raw.substr(body_at + 4);
        if (!body.empty()) response.json = nlohmann::json::parse(body);
    }
    return response;
}

ScgiRequest get_request(std::string uri) {
    return {.method = "GET", .document_uri = std::move(uri)};
}

ScgiRequest post_request(
    std::string uri,
    std::string body,
    std::string type = std::string(json_type)) {
    return {
        .method = "POST",
        .document_uri = std::move(uri),
        .body = std::move(body),
        .content_type = std::move(type),
    };
}

class ChaWebAdapterTest : public testing::Test {
protected:
    void SetUp() override { open_application(); }

    void open_application(RuntimeSettings settings = {}) {
        database_ = test::import_test_database(workspace_.root());
        application_ = Application::open(
            make_command(workspace_, database_), {}, std::move(settings));
    }

    CgiResponse get(std::string uri) {
        return exchange(*application_, get_request(std::move(uri)));
    }

    CgiResponse post(
        std::string uri,
        const nlohmann::json& body,
        std::string type = std::string(json_type),
        ChaWebDeleteSession delete_session = nullptr) {
        return exchange(
            *application_,
            post_request(std::move(uri), body.dump(), std::move(type)),
            delete_session);
    }

    std::string create_session(std::string_view text = "@- note") {
        const CgiResponse created = post(
            sessions_path("lobby"), text_body(text));
        EXPECT_EQ(created.status, 201) << created.raw;
        return created.json.at("id").get<std::string>();
    }

    std::vector<SessionListing> listed(std::string_view forum = "lobby") {
        return application_->list_sessions(
            forum, application_->context_epoch());
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::unique_ptr<Application> application_;
};

TEST(ChaWebAdapter, MatchesRoutePrefixAtItsBoundary) {
    EXPECT_TRUE(is_chaweb_request("/api/cha/v1"));
    EXPECT_TRUE(is_chaweb_request("/api/cha/v1/"));
    EXPECT_TRUE(is_chaweb_request("/api/cha/v1/bootstrap"));
    EXPECT_FALSE(is_chaweb_request("/api/cha/v1foo"));
    EXPECT_FALSE(is_chaweb_request("/v1/models"));
    EXPECT_FALSE(is_chaweb_request("/api/cha/v2/bootstrap"));
}

TEST_F(ChaWebAdapterTest, BootstrapUsesExistingSerializer) {
    const auto boot = application_->bootstrap();
    const CgiResponse response = get(std::string(bootstrap_path));
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.json, nlohmann::json(boot.presentation));
    EXPECT_EQ(response.json.at("entrance_forum_id"), std::string(entrance_id));
    EXPECT_FALSE(response.json.contains("state"));
    EXPECT_FALSE(response.json.contains("context_epoch"));
    EXPECT_NE(response.raw.find("Content-Type: application/json"), std::string::npos);
}

TEST_F(ChaWebAdapterTest, ListsSessionsAndRejectsUnknownForums) {
    const std::string id = create_session();
    const auto epoch = application_->context_epoch();
    const CgiResponse listed_response = get(sessions_path("lobby"));
    EXPECT_EQ(listed_response.status, 200);
    EXPECT_EQ(
        listed_response.json,
        nlohmann::json(application_->list_sessions("lobby", epoch)));
    ASSERT_EQ(listed_response.json.size(), 1u);
    EXPECT_EQ(listed_response.json.at(0).at("id"), id);

    const CgiResponse missing = get(sessions_path("missing"));
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.json.at("error").at("code"), "not_found");
    EXPECT_FALSE(missing.json.at("error").at("message").get<std::string>().empty());
}

TEST_F(ChaWebAdapterTest, CreatesOpensInputsAndStops) {
    const CgiResponse created =
        post(sessions_path("lobby"), text_body("@- Hello"));
    ASSERT_EQ(created.status, 201) << created.raw;
    const std::string id = created.json.at("id").get<std::string>();
    EXPECT_EQ(
        created.json,
        nlohmann::json(CreateSessionSuccess{
            id, created.json.at("label").get<std::string>()}));

    const CgiResponse snapshot = get(session_path("lobby", id));
    ASSERT_EQ(snapshot.status, 200) << snapshot.raw;
    EXPECT_EQ(snapshot.json.at("session_id"), id);
    EXPECT_TRUE(snapshot.json.contains("generation"));
    EXPECT_TRUE(snapshot.json.contains("transcript"));
    const auto stored = application_->snapshot(
        "lobby", id, application_->context_epoch());
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(stored));
    EXPECT_EQ(snapshot.json, nlohmann::json(std::get<SessionSnapshot>(stored)));

    const CgiResponse again =
        post(session_path("lobby", id) + "/input", text_body("@- again"));
    EXPECT_EQ(again.status, 204) << again.raw;
    EXPECT_EQ(again.raw, "Status: 204 No Content\r\n\r\n");

    const CgiResponse stopped =
        post(session_path("lobby", id) + "/stop", nlohmann::json::object());
    EXPECT_EQ(stopped.status, 204) << stopped.raw;

    const CgiResponse idle =
        post(session_path("lobby", id) + "/stop", nlohmann::json::object());
    EXPECT_EQ(idle.status, 204) << idle.raw;
}

TEST_F(ChaWebAdapterTest, UnknownRoutesAndMethodsAreNotFoundWithoutCors) {
    const struct Case {
        const char* method;
        const char* uri;
    } cases[]{
        {"GET", "/api/cha/v1"},
        {"GET", "/api/cha/v1/"},
        {"GET", "/api/cha/v1/unknown"},
        {"POST", "/api/cha/v1/bootstrap"},
        {"OPTIONS", "/api/cha/v1/bootstrap"},
        {"GET", "/api/cha/v1/forums/lobby/sessions/abc/input"},
        {"POST", "/api/cha/v1/forums/lobby/sessions/abc"},
        {"GET", "/api/cha/v1/forums/../sessions"},
        {"GET", "/api/cha/v1/forums/lobby/sessions/has/slash"},
        {"GET", "/api/cha/v1/forums/lobby/sessions/"},
        {"GET", "/api/cha/v1/forums/not valid/sessions"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(std::string(item.method) + " " + item.uri);
        const CgiResponse response = exchange(
            *application_,
            {.method = item.method, .document_uri = item.uri, .body = "{}"});
        EXPECT_EQ(response.status, 404);
        EXPECT_EQ(response.json.at("error").at("code"), "not_found");
        EXPECT_EQ(response.raw.find("Allow:"), std::string::npos);
        EXPECT_EQ(response.raw.find("Access-Control"), std::string::npos);
        EXPECT_FALSE(response.json.contains("type"));
    }
}

TEST_F(ChaWebAdapterTest, RejectsMalformedBodiesAndUnknownFields) {
    const std::string id = create_session();
    const struct Case {
        std::string uri;
        std::string body;
    } cases[]{
        {sessions_path("lobby"), "{"},
        {sessions_path("lobby"), "[]"},
        {sessions_path("lobby"), "\"text\""},
        {sessions_path("lobby"), "{\"text\":1}"},
        {sessions_path("lobby"), "{\"text\":\"hi\",\"extra\":true}"},
        {session_path("lobby", id) + "/input", "{\"text\":\"hi\",\"n\":1}"},
        {session_path("lobby", id) + "/stop", "{\"reason\":\"x\"}"},
        {session_path("lobby", id) + "/stop", "[]"},
    };
    const auto before = listed().size();
    for (const auto& item : cases) {
        SCOPED_TRACE(item.body);
        const CgiResponse response = exchange(
            *application_, post_request(item.uri, item.body));
        EXPECT_EQ(response.status, 400);
        EXPECT_EQ(response.json.at("error").at("code"), "invalid_argument");
    }
    EXPECT_EQ(listed().size(), before);
}

TEST_F(ChaWebAdapterTest, ForumSessionMismatchAndUnknownStopAreNotFound) {
    workspace_.add_forum("stoics", "Stoics", "guide");
    application_.reset();
    open_application();
    const std::string id = create_session();

    const CgiResponse mismatch = get(session_path("stoics", id));
    EXPECT_EQ(mismatch.status, 404);
    EXPECT_EQ(mismatch.json.at("error").at("code"), "not_found");

    const CgiResponse unknown = post(
        session_path("lobby", "missing") + "/stop", nlohmann::json::object());
    EXPECT_EQ(unknown.status, 404);
}

TEST_F(ChaWebAdapterTest, PromptLimitIsCheckedBeforeCreation) {
    RuntimeSettings settings;
    settings.prompt_limit = 8;
    application_.reset();
    open_application(settings);
    const std::size_t limit = application_->settings().prompt_limit;
    const CgiResponse allowed = post(
        sessions_path("lobby"), text_body(std::string(limit, 'a')));
    EXPECT_EQ(allowed.status, 201) << allowed.raw;

    const auto before = listed().size();
    const CgiResponse rejected = post(
        sessions_path("lobby"), text_body(std::string(limit + 1, 'a')));
    EXPECT_EQ(rejected.status, 400);
    EXPECT_EQ(rejected.json.at("error").at("code"), "prompt_too_large");
    EXPECT_EQ(listed().size(), before);

    const std::string id = allowed.json.at("id").get<std::string>();
    const CgiResponse input = post(
        session_path("lobby", id) + "/input",
        text_body(std::string(limit + 1, 'b')));
    EXPECT_EQ(input.status, 400);
    EXPECT_EQ(input.json.at("error").at("code"), "prompt_too_large");
}

TEST_F(ChaWebAdapterTest, PostMediaTypesAreValidatedBeforeMutation) {
    const std::string existing = create_session();
    const std::string posts[]{
        sessions_path("lobby"),
        session_path("lobby", existing) + "/input",
        session_path("lobby", existing) + "/stop",
    };
    const char* accepted[]{
        "application/json",
        "Application/JSON",
        " application/json ",
        "application/json; charset=utf-8",
        "application/json;charset=UTF-8",
    };
    const char* rejected[]{
        "",
        "text/plain",
        "application/jsonx",
        "application/jsonp",
        "application/jsonyes",
        "json",
        "text/application/json",
    };

    for (const char* type : accepted) {
        SCOPED_TRACE(type);
        const CgiResponse create = post(
            sessions_path("lobby"), text_body("@- Hello"), type);
        EXPECT_EQ(create.status, 201) << create.raw;
        const std::string id = create.json.at("id").get<std::string>();
        EXPECT_EQ(
            post(
                session_path("lobby", id) + "/input",
                text_body("@- Hi"),
                type)
                .status,
            204);
        EXPECT_EQ(
            post(
                session_path("lobby", id) + "/stop",
                nlohmann::json::object(),
                type)
                .status,
            204);
    }

    const auto before = listed().size();
    for (const auto& path : posts) {
        for (const char* type : rejected) {
            SCOPED_TRACE(std::string(path) + " " + type);
            const nlohmann::json body = std::string_view(path).ends_with("/stop")
                ? nlohmann::json::object()
                : text_body("should not store");
            const CgiResponse response = post(path, body, type);
            EXPECT_EQ(response.status, 415);
            EXPECT_EQ(response.json.at("error").at("code"), "invalid_argument");
        }
        const CgiResponse malformed = exchange(
            *application_,
            post_request(path, "{", std::string(json_type)));
        EXPECT_EQ(malformed.status, 400);
        EXPECT_EQ(malformed.json.at("error").at("code"), "invalid_argument");
    }
    EXPECT_EQ(listed().size(), before);
}

TEST_F(ChaWebAdapterTest, AcceptsSelfNotesAndCleansUpParserRejection) {
    const CgiResponse note =
        post(sessions_path("lobby"), text_body("@- keep this note"));
    ASSERT_EQ(note.status, 201) << note.raw;
    const std::string id = note.json.at("id").get<std::string>();
    const CgiResponse snapshot = get(session_path("lobby", id));
    ASSERT_EQ(snapshot.status, 200);
    bool found = false;
    for (const auto& entry : snapshot.json.at("transcript")) {
        if (entry.at("text").get<std::string>().find("keep this note")
            != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);

    const auto before = listed().size();
    const CgiResponse rejected =
        post(sessions_path("lobby"), text_body("/not-a-command"));
    EXPECT_EQ(rejected.status, 422);
    EXPECT_EQ(rejected.json.at("error").at("code"), "invalid_argument");
    EXPECT_NE(
        rejected.json.at("error").at("message").get<std::string>().find(
            "Unknown command"),
        std::string::npos);
    EXPECT_EQ(listed().size(), before);

    const CgiResponse later = post(
        session_path("lobby", id) + "/input", text_body("/still-unknown"));
    EXPECT_EQ(later.status, 422);
    EXPECT_EQ(listed().size(), before);
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(
        application_->snapshot("lobby", id, application_->context_epoch())));
}

TEST_F(ChaWebAdapterTest, CleanupFailureReturns500AndUnknownDeleteIsSuccess) {
    const auto fail = [](Application&,
                         std::string_view,
                         std::string_view,
                         std::uint64_t) -> std::optional<ErrorCode> {
        return ErrorCode::internal_error;
    };
    const auto gone = [](Application& application,
                         std::string_view forum_id,
                         std::string_view session_id,
                         std::uint64_t epoch) -> std::optional<ErrorCode> {
        (void)application.delete_session(forum_id, session_id, epoch);
        return ErrorCode::not_found;
    };

    const CgiResponse failed = post(
        sessions_path("lobby"),
        text_body("/not-a-command"),
        std::string(json_type),
        fail);
    EXPECT_EQ(failed.status, 500);
    EXPECT_EQ(failed.json.at("error").at("code"), "internal_error");
    EXPECT_FALSE(listed().empty());

    const auto before = listed().size();
    const CgiResponse cleaned = post(
        sessions_path("lobby"),
        text_body("/not-a-command"),
        std::string(json_type),
        gone);
    EXPECT_EQ(cleaned.status, 422);
    EXPECT_EQ(listed().size(), before);
}

TEST_F(ChaWebAdapterTest, LoadsARetiredSessionAndDoesNotRetryUnknownOnes) {
    const std::string id = create_session();
    const auto epoch = application_->context_epoch();
    application_->close_session("lobby", id, epoch);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (application_->live_session_count() > 0
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(application_->live_session_count(), 0u);

    const CgiResponse snapshot = get(session_path("lobby", id));
    ASSERT_EQ(snapshot.status, 200) << snapshot.raw;
    EXPECT_EQ(snapshot.json.at("session_id"), id);

    const auto before = listed().size();
    const CgiResponse missing = post(
        session_path("lobby", "missing") + "/input", text_body("Hello"));
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(listed().size(), before);
}

TEST_F(ChaWebAdapterTest, UnavailableApplicationReturns503) {
    application_->mark_unusable();
    const CgiResponse response = get(std::string(bootstrap_path));
    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(
        response.json.at("error").at("code"), "application_unavailable");
}

TEST(ChaWebAdapter, UnknownSubmissionOutcomeKeepsTheSession) {
    test::TestWorkspace workspace;
    MockHttpServer server({http_json(
        R"({"answers":{"recipient":{"type":"choice","choice":"undefined"}}})")});
    server.pause_before_response(1);
    const auto directory = workspace.root() / "system/jev";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml")
        << "url = \"http://127.0.0.1:" << server.port()
        << "/decisions\"\nmodel = \"jev\"\napi_key = \"api_key_1\"\n";
    RuntimeSettings settings;
    settings.command_deadline = 200ms;
    auto application = Application::open(
        make_command(workspace, test::import_test_database(workspace.root())),
        {},
        settings);
    ASSERT_EQ(
        application->create_api_key(
            {.display_name = "Jev", .value = "jev-secret"},
            application->context_epoch())
            .id,
        "api_key_1");
    server.start();
    CgiResponse response;
    std::thread worker([&] {
        response = exchange(
            *application,
            post_request(sessions_path("lobby"), text_body("Hello").dump()));
    });
    ASSERT_TRUE(server.wait_for_requests(1, 5s));
    worker.join();
    EXPECT_EQ(response.status, 500) << response.raw;
    EXPECT_EQ(response.json.at("error").at("code"), "command_timeout");
    EXPECT_FALSE(
        application->list_sessions("lobby", application->context_epoch())
            .empty());
    server.resume_responses();
    server.join();
}

TEST(ChaWebAdapter, InputReturnsWhileGenerationContinues) {
    test::TestWorkspace workspace;
    disable_naming(workspace);
    MockHttpServer server({http_json(
        R"({"choices":[{"message":{"content":"Later"}}]})")});
    server.pause_before_response(1);
    use_net_provider(workspace, server.port());
    auto application = Application::open(
        make_command(workspace, test::import_test_database(workspace.root())));
    server.start();
    const CgiResponse created = exchange(
        *application,
        post_request(sessions_path("lobby"), text_body("Hello").dump()));
    ASSERT_EQ(created.status, 201) << created.raw;
    ASSERT_TRUE(server.wait_for_requests(1, 5s));
    const std::string id = created.json.at("id").get<std::string>();
    const CgiResponse snapshot = exchange(
        *application, get_request(session_path("lobby", id)));
    ASSERT_EQ(snapshot.status, 200) << snapshot.raw;
    EXPECT_TRUE(snapshot.json.at("generation").at("active").get<bool>());
    server.resume_responses();
    server.join();
}

} // namespace
} // namespace cha::daemon
