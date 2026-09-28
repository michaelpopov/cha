#include "app/application.h"
#include "app/application_config.h"
#include "daemon/openai_adapter.h"
#include "daemon/scgi.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace cha {
namespace {

constexpr int listen_fd = 3;
constexpr const char daemon_usage[] =
    "Usage:\n"
    "  cha-daemon --config=CONFIG_DIR\n";

std::atomic<bool> stop_requested{false};
static_assert(std::atomic<bool>::is_always_lock_free);

void handle_stop(int) {
    stop_requested.store(true, std::memory_order_relaxed);
}

std::runtime_error argument_error(std::string message) {
    return std::runtime_error(std::move(message) + "\n" + daemon_usage);
}

std::filesystem::path parse_config_directory(int argc, char** argv) {
    std::optional<std::filesystem::path> config;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const std::size_t equals = argument.find('=');
        const std::string_view option = argument.substr(0, equals);
        if (option != "--config") {
            throw argument_error(
                "Unknown option '" + std::string(option) + "'.");
        }
        std::string_view value;
        if (equals != std::string_view::npos) {
            value = argument.substr(equals + 1);
        } else {
            if (++index >= argc) {
                throw argument_error(
                    "Option '--config' requires a value.");
            }
            value = argv[index];
        }
        if (config) {
            throw argument_error("Option '--config' was provided more than once.");
        }
        if (value.empty()) {
            throw argument_error("Option '--config' requires a non-empty path.");
        }
        config = std::filesystem::weakly_canonical(
            std::filesystem::absolute(path_from_utf8(value)));
    }
    if (!config) {
        throw argument_error("Missing --config=CONFIG_DIR.");
    }
    return *config;
}

ApplicationCommand load_daemon_command(const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error(
            "Configuration directory '" + utf8_path(directory)
            + "' requires an existing directory.");
    }
    const ConfigurationDirectory settings =
        load_configuration_directory(directory);
    const VaultDefinition* const selected =
        find_vault(settings.vaults, settings.startup_vault);
    if (selected == nullptr) {
        throw std::runtime_error(
            "Application config '"
            + utf8_path(settings.directory / "app.toml")
            + "' field 'vault' does not name a discovered vault.");
    }
    if (!std::filesystem::is_regular_file(selected->data)) {
        throw std::runtime_error(
            "Workspace session database '" + utf8_path(selected->data)
            + "' does not exist.");
    }
    if (!selected->password_protected) {
        require_switchable_database(selected->data);
    }
    return {
        .config_directory = settings.directory,
        .mirror_base = settings.mirror_base,
        .modify_base = settings.modify_base,
        .vaults = settings.vaults,
        .vault = *selected,
        .log_file = settings.log_file,
        .log_level = settings.log_level,
        .warnings = settings.warnings,
    };
}

void require_activation() {
    const char* const pid_text = std::getenv("LISTEN_PID");
    const char* const fds_text = std::getenv("LISTEN_FDS");
    if (pid_text == nullptr || fds_text == nullptr) {
        throw std::runtime_error(
            "systemd socket activation is invalid: LISTEN_PID and LISTEN_FDS "
            "are required.");
    }
    errno = 0;
    char* end = nullptr;
    const long pid = std::strtol(pid_text, &end, 10);
    if (errno != 0 || end == pid_text || *end != '\0' || pid != ::getpid()) {
        throw std::runtime_error(
            "systemd socket activation is invalid: LISTEN_PID does not match "
            "this process.");
    }
    if (std::strcmp(fds_text, "1") != 0) {
        throw std::runtime_error(
            "systemd socket activation is invalid: LISTEN_FDS must be 1.");
    }

    struct stat status {};
    if (::fstat(listen_fd, &status) != 0 || !S_ISSOCK(status.st_mode)) {
        throw std::runtime_error(
            "systemd socket activation is invalid: file descriptor 3 is not a "
            "listening Unix stream socket.");
    }
    int type = 0;
    socklen_t type_size = sizeof(type);
    if (::getsockopt(listen_fd, SOL_SOCKET, SO_TYPE, &type, &type_size) != 0
        || type != SOCK_STREAM) {
        throw std::runtime_error(
            "systemd socket activation is invalid: file descriptor 3 is not a "
            "listening Unix stream socket.");
    }
    // macOS defines SO_ACCEPTCONN but getsockopt returns ENOPROTOOPT for it.
#ifndef __APPLE__
    int accepting = 0;
    socklen_t accepting_size = sizeof(accepting);
    if (::getsockopt(
            listen_fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_size)
            != 0
        || accepting == 0) {
        throw std::runtime_error(
            "systemd socket activation is invalid: file descriptor 3 is not a "
            "listening Unix stream socket.");
    }
#endif
    sockaddr_storage address {};
    socklen_t address_size = sizeof(address);
    if (::getsockname(
            listen_fd, reinterpret_cast<sockaddr*>(&address), &address_size)
            != 0
        || address.ss_family != AF_UNIX) {
        throw std::runtime_error(
            "systemd socket activation is invalid: file descriptor 3 is not a "
            "listening Unix stream socket.");
    }
    if (!daemon::configure_socket(listen_fd)) {
        throw std::runtime_error(
            "systemd socket activation is invalid: file descriptor 3 could not "
            "be configured.");
    }
    ::unsetenv("LISTEN_PID");
    ::unsetenv("LISTEN_FDS");
    ::unsetenv("LISTEN_FDNAMES");
}

std::string read_vault_password(const std::filesystem::path& directory) {
    const std::filesystem::path file = directory / "password";
    daemon::UniqueFd fd(::open(
        file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd) {
        throw std::runtime_error(
            "Protected vault requires a private password file '"
            + utf8_path(file) + "'.");
    }
    struct stat status {};
    if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
        throw std::runtime_error(
            "Vault password path '" + utf8_path(file)
            + "' must be a regular file.");
    }
    if ((status.st_mode & 0777) != 0600) {
        throw std::runtime_error(
            "Vault password file '" + utf8_path(file) + "' must have mode 0600.");
    }
    if (status.st_size < 0 || status.st_size > 4096) {
        throw std::runtime_error(
            "Vault password file '" + utf8_path(file) + "' is invalid.");
    }
    std::string password(static_cast<std::size_t>(status.st_size), '\0');
    std::size_t received = 0;
    while (received < password.size()) {
        const ssize_t count = ::read(
            fd.get(), password.data() + received, password.size() - received);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(
                "Failed to read vault password file '" + utf8_path(file) + "'.");
        }
        if (count == 0) break;
        received += static_cast<std::size_t>(count);
    }
    password.resize(received);
    if (password.size() >= 2 && password.ends_with("\r\n")) {
        password.resize(password.size() - 2);
    } else if (!password.empty()
        && (password.back() == '\n' || password.back() == '\r')) {
        password.pop_back();
    }
    if (password.empty()) {
        throw std::runtime_error(
            "Vault password file '" + utf8_path(file) + "' is empty.");
    }
    return password;
}

void install_stop_handlers() {
    struct sigaction action {};
    action.sa_handler = handle_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGTERM, &action, nullptr) != 0
        || ::sigaction(SIGINT, &action, nullptr) != 0) {
        throw std::runtime_error("Failed to install stop signal handlers.");
    }
}

// Serves one connection at a time. New connections wait in the socket
// backlog until the current one is closed.
void run_accept_loop(app::Application& application) {
    using daemon::ScgiReadStatus;
    while (!stop_requested.load()) {
        const daemon::UniqueFd client =
            daemon::accept_connection(listen_fd, stop_requested);
        if (!client) continue;
        const daemon::ScgiReadResult result =
            daemon::read_scgi(client.get(), stop_requested);
        if (result.status == ScgiReadStatus::bad_request) {
            daemon::write_error(
                client.get(),
                {.status = 400,
                 .message = "Malformed SCGI request",
                 .code = "invalid_request"},
                stop_requested);
        } else if (result.status == ScgiReadStatus::too_large) {
            daemon::write_error(
                client.get(),
                {.status = 413,
                 .message = "The request is too large",
                 .code = "body_too_large"},
                stop_requested);
        } else if (result.status == ScgiReadStatus::ok
            && !stop_requested.load() && !daemon::peer_closed(client.get())) {
            daemon::handle_request(
                application, result.request, client.get(), stop_requested);
        }
    }
}

void report_error(std::string_view message, bool logging_ready) {
    std::cerr << message << '\n';
    if (logging_ready) log_error(message);
}

} // namespace
} // namespace cha

int main(int argc, char** argv) {
    bool logging_ready = false;
    try {
        const std::filesystem::path config_directory =
            cha::parse_config_directory(argc, argv);
        cha::require_activation();
        cha::ApplicationCommand command =
            cha::load_daemon_command(config_directory);
        cha::initialize_diagnostic_logging(command.log_file, command.log_level);
        logging_ready = true;
        cha::install_stop_handlers();

        std::string password;
        if (command.vault.password_protected) {
            password = cha::read_vault_password(command.config_directory);
        }

        auto application = cha::app::Application::open(
            command, std::move(password));
        cha::run_accept_loop(*application);
        // This also cancels a turn that was running when SIGTERM came.
        application->request_shutdown();
        const auto grace = application->settings().shutdown_grace;
        if (!application->join_shutdown(grace)) {
            cha::report_error("CHA application shutdown timed out", true);
            _exit(EXIT_FAILURE);
        }
        application.reset();
        cha::shutdown_diagnostic_logging();
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        cha::report_error(error.what(), logging_ready);
        if (logging_ready) cha::shutdown_diagnostic_logging();
        return EXIT_FAILURE;
    }
}
