#include "daemon/scgi.h"
#include "daemon_process.h"
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
#include <thread>
#include <vector>

#include <sys/socket.h>

namespace cha::test {
namespace {

using namespace std::chrono_literals;

std::filesystem::path write_config(
    const TestWorkspace& workspace, const std::filesystem::path& database) {
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
    }
    return config;
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
        .config_directory = config_,
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
    ASSERT_TRUE(client);
    const std::string headers =
        std::string("CONTENT_LENGTH") + '\0' + "0" + '\0'
        + "SCGI" + '\0' + "1" + '\0'
        + "REQUEST_METHOD" + '\0' + "GET" + '\0'
        + "DOCUMENT_URI" + '\0' + "/v1/models" + '\0';
    const std::string request =
        std::to_string(headers.size()) + ":" + headers + ",";
    std::atomic<bool> stop{false};
    ASSERT_TRUE(daemon::write_bytes(client.get(), request, stop));
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
    EXPECT_NE(raw.find("Status: 200 OK"), std::string::npos);
    EXPECT_NE(raw.find("\"id\":\"lobby\""), std::string::npos);
    EXPECT_EQ(raw.find("builtin-entrance"), std::string::npos);
    process.send_signal(SIGTERM);
    EXPECT_EQ(process.wait_for_exit(5s), 0);
}

} // namespace
} // namespace cha::test
