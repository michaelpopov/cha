#include "daemon/scgi.h"
#include "daemon_process.h"
#include "storage/workspace_session_database.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/socket.h>

namespace cha::test {
namespace {

using namespace std::chrono_literals;

std::filesystem::path write_config(
    const TestWorkspace& workspace,
    const std::filesystem::path& database,
    bool protected_vault = false) {
    const std::filesystem::path config = workspace.root() / "cha-config";
    std::filesystem::create_directories(config);
    {
        std::ofstream app(config / "app.toml");
        app << "vault = \"Test\"\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(database.string()) << "\n";
        if (protected_vault) vault << "protected = true\n";
    }
    return config;
}

// Sends GET /v1/models to a starting daemon and returns the raw response.
std::string request_models(DaemonProcess& process) {
    daemon::UniqueFd client;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            client = process.connect_client();
            break;
        } catch (const std::exception&) {
            std::this_thread::sleep_for(20ms);
        }
    }
    EXPECT_TRUE(client);
    if (!client) return {};
    const std::string headers =
        std::string("CONTENT_LENGTH") + '\0' + "0" + '\0'
        + "SCGI" + '\0' + "1" + '\0'
        + "REQUEST_METHOD" + '\0' + "GET" + '\0'
        + "DOCUMENT_URI" + '\0' + "/v1/models" + '\0';
    const std::string request =
        std::to_string(headers.size()) + ":" + headers + ",";
    std::atomic<bool> stop{false};
    EXPECT_TRUE(daemon::write_bytes(client.get(), request, stop));
    std::string raw;
    char buffer[4096];
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t count = ::recv(client.get(), buffer, sizeof(buffer), 0);
        if (count > 0) {
            raw.append(buffer, static_cast<std::size_t>(count));
            if (raw.find("\r\n\r\n") != std::string::npos
                && raw.find('}') != std::string::npos) {
                break;
            }
            continue;
        }
        if (count == 0) break;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            std::this_thread::sleep_for(10ms);
            continue;
        }
        break;
    }
    return raw;
}

class DaemonProcessTest : public testing::Test {
protected:
    void SetUp() override {
        database_ = import_test_database(workspace_.root());
        config_ = write_config(workspace_, database_);
    }

    TestWorkspace workspace_;
    std::filesystem::path database_;
    std::filesystem::path config_;
};

TEST_F(DaemonProcessTest, RejectsInvalidActivation) {
    struct Case {
        const char* name;
        DaemonSpawn spawn;
    };
    std::vector<Case> cases;
    cases.push_back({"missing listen pid", DaemonSpawn{
        .config_directory = workspace_.root() / "missing-config",
        .set_listen_pid = false,
    }});
    cases.push_back({"wrong listen pid", DaemonSpawn{
        .config_directory = config_,
        .listen_pid = "1",
    }});
    cases.push_back({"listen fds not one", DaemonSpawn{
        .config_directory = config_,
        .listen_fds = "2",
    }});
    cases.push_back({"fd 3 is not a socket", DaemonSpawn{
        .config_directory = config_,
        .pass_listen_socket = false,
    }});
    cases.push_back({"missing listen fds", DaemonSpawn{
        .config_directory = config_,
        .set_listen_fds = false,
        .equals_config_option = false,
    }});

    for (auto& item : cases) {
        SCOPED_TRACE(item.name);
        DaemonProcess process(item.spawn);
        const int status = process.wait_for_exit(5s);
        EXPECT_NE(status, 0);
        EXPECT_NE(
            process.stderr_text().find("systemd socket activation is invalid"),
            std::string::npos);
    }
}

TEST_F(DaemonProcessTest, ServesModelsAfterActivation) {
    DaemonProcess process(DaemonSpawn{.config_directory = config_});
    const std::string raw = request_models(process);
    EXPECT_NE(raw.find("Status: 200 OK"), std::string::npos);
    EXPECT_NE(raw.find("\"id\":\"The Lobby\""), std::string::npos);
    EXPECT_EQ(raw.find("builtin-entrance"), std::string::npos);
    process.send_signal(SIGTERM);
    EXPECT_EQ(process.wait_for_exit(5s), 0);
}

TEST_F(DaemonProcessTest, ProtectedVaultReadsThePasswordFile) {
    using std::filesystem::perms;
    protect_workspace_session_database(database_, "secret");
    config_ = write_config(workspace_, database_, true);
    const std::filesystem::path password = config_ / "password";
    const perms private_mode = perms::owner_read | perms::owner_write;
    const auto write_file = [](const std::filesystem::path& file,
                               std::string_view text,
                               perms mode) {
        std::filesystem::remove(file);
        std::ofstream(file) << text;
        std::filesystem::permissions(file, mode);
    };

    struct Case {
        const char* name;
        const char* error;  // nullptr: any startup error
    };
    const auto expect_startup_error = [&](const Case& item) {
        SCOPED_TRACE(item.name);
        DaemonProcess process(DaemonSpawn{.config_directory = config_});
        EXPECT_NE(process.wait_for_exit(5s), 0);
        const std::string text = process.stderr_text();
        if (item.error != nullptr) {
            EXPECT_NE(text.find(item.error), std::string::npos) << text;
        } else {
            EXPECT_FALSE(text.empty());
        }
    };

    expect_startup_error({"missing file", "requires a private password file"});
    const std::filesystem::path target = workspace_.root() / "password-target";
    write_file(target, "secret\n", private_mode);
    std::filesystem::create_symlink(target, password);
    expect_startup_error({"symbolic link", "requires a private password file"});
    write_file(password, "secret\n",
               private_mode | perms::group_read | perms::others_read);
    expect_startup_error({"readable by others", "must have mode 0600"});
    write_file(password, "", private_mode);
    expect_startup_error({"empty file", "is empty"});
    write_file(password, "\n", private_mode);
    expect_startup_error({"only a newline", "is empty"});
    write_file(password, "wrong\n", private_mode);
    expect_startup_error({"wrong password", nullptr});

    // One trailing newline, as `echo` writes it, is not part of the password.
    write_file(password, "secret\n", private_mode);
    DaemonProcess process(DaemonSpawn{.config_directory = config_});
    const std::string raw = request_models(process);
    EXPECT_NE(raw.find("Status: 200 OK"), std::string::npos) << raw;
    EXPECT_NE(raw.find("\"id\":\"The Lobby\""), std::string::npos) << raw;
    process.send_signal(SIGTERM);
    EXPECT_EQ(process.wait_for_exit(5s), 0);
}

TEST(DaemonProcess, StopsDuringGeneration) {
    TestWorkspace workspace;
    const auto session_dir = workspace.root() / "system/session";
    std::filesystem::create_directories(session_dir);
    std::ofstream(session_dir / "config.toml")
        << "naming_provider = \"absent\"\n";
    const std::string body =
        R"({"choices":[{"message":{"content":"Finished"}}]})";
    MockHttpServer server({http_response("application/json", body)});
    server.pause_before_response(1);
    workspace.write_provider(
        "remote",
        "host = \"127.0.0.1\"\nport = " + std::to_string(server.port())
            + "\nhttps = false\nmode = \"net\"\nmodel = \"fake\"\n"
              "api = \"chat_completions\"\nstream = false\ntimeout_s = 20\n");
    workspace.write_character_config(
        "display_name = \"Guide\"\nprovider = \"remote\"\n");
    const auto database = import_test_database(workspace.root());
    const auto config = write_config(workspace, database);
    DaemonProcess process(DaemonSpawn{.config_directory = config});
    server.start();

    daemon::UniqueFd client;
    const auto ready = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < ready) {
        try {
            client = process.connect_client();
            break;
        } catch (const std::exception&) {
            std::this_thread::sleep_for(20ms);
        }
    }
    ASSERT_TRUE(client);
    const std::string payload =
        R"({"model":"lobby","stream":true,"messages":[{"role":"user","content":"Hello"}]})";
    const std::string headers =
        std::string("CONTENT_LENGTH") + '\0' + std::to_string(payload.size())
        + '\0' + "SCGI" + '\0' + "1" + '\0' + "REQUEST_METHOD" + '\0' + "POST"
        + '\0' + "DOCUMENT_URI" + '\0' + "/v1/chat/completions" + '\0';
    const std::string request =
        std::to_string(headers.size()) + ":" + headers + "," + payload;
    std::atomic<bool> stop{false};
    ASSERT_TRUE(daemon::write_bytes(client.get(), request, stop));

    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (raw.find("text/event-stream") == std::string::npos
        && std::chrono::steady_clock::now() < deadline) {
        char buffer[1024];
        const ssize_t count =
            ::recv(client.get(), buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count > 0) raw.append(buffer, static_cast<std::size_t>(count));
        else std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(raw.find("text/event-stream"), std::string::npos) << raw;
    ASSERT_TRUE(server.wait_for_requests(1, 5s));
    process.send_signal(SIGTERM);
    server.resume_responses();
    EXPECT_EQ(process.wait_for_exit(15s), 0);
    server.join();
}

} // namespace
} // namespace cha::test
