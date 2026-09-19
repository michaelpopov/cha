#pragma once

#include "web/application_config.h"
#include "web/command_queue.h"
#include "web/live_session_manager.h"
#include "web/protocol.h"
#include "web/r2_database_transfer.h"
#include "web/web_settings.h"
#include "workspace/workspace_config_store.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace cha {
class SessionRepository;
class Providers;
class ApiKeyStore;
class OpenAiOAuth;
}

namespace cha::web {
class CurrentVault;
class SessionMirror;
class AudioDownloadManager;

struct VaultCreate {
    std::string display_name;
    std::optional<std::string> copy_from;
    std::string password;
};

struct VaultUpdate {
    std::string display_name;
    std::string password;
};

struct VaultRegistrySnapshot {
    std::vector<VaultDefinition> vaults;
    VaultDefinition active;
};
}

namespace cha::app {

enum class ApplicationState {
    running,
    maintenance,
    stopping,
    unavailable,
};

struct ApplicationBootstrap {
    ApplicationState state{ApplicationState::running};
    std::uint64_t context_epoch{1};
    cha::web::Bootstrap presentation;
};

struct MaintenanceResult {
    ApplicationState state{ApplicationState::running};
    std::uint64_t context_epoch{1};
};

struct ApplicationCapabilities {
    bool can_modify{};
    bool can_transfer_r2{};
};

struct ResourceHooks {
    std::function<void(bool cancel)> pause;
    std::function<void()> resume;
};

using ContextChanged = std::function<
    void(std::uint64_t context_epoch, ApplicationState state)>;

class ApplicationError : public std::runtime_error {
public:
    explicit ApplicationError(
        cha::web::ErrorCode code,
        std::string message = {});
    cha::web::ErrorCode code;
};

[[nodiscard]] std::string_view application_state_name(
    ApplicationState state) noexcept;

cha::web::WebSettings native_settings();

// Transport-neutral composition root. Construction does not bind a port or
// start a listener. The temporary HTTP runtime wraps this object.
class Application {
public:
    static std::unique_ptr<Application> open(
        const cha::web::ApplicationCommand& command,
        std::string vault_password = {},
        cha::web::WebSettings settings = native_settings());

    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] ApplicationBootstrap bootstrap();
    [[nodiscard]] cha::web::CreateSessionSuccess create_session(
        std::string_view forum_id,
        std::string label,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::variant<
        cha::web::OpenSessionSuccess,
        cha::web::ErrorCode>
    open_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult submit(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::WebCommand command,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::variant<
        std::shared_ptr<cha::web::CommandReply>,
        cha::web::ErrorCode>
    submit_async(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::WebCommand command,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult stop(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult snapshot(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult subscribe(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::SubscribeCommand command,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult unsubscribe(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::UnsubscribeCommand command,
        std::uint64_t epoch = 0);
    void close_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::optional<cha::web::ErrorCode> delete_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);

    [[nodiscard]] cha::web::VaultRegistrySnapshot vault_snapshot() const;
    [[nodiscard]] cha::web::VaultDefinition create_vault(
        cha::web::VaultCreate create,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::VaultDefinition update_vault(
        std::string_view current_name,
        cha::web::VaultUpdate update,
        std::uint64_t epoch = 0);
    void delete_vault(std::string_view name, std::uint64_t epoch = 0);
    [[nodiscard]] MaintenanceResult switch_vault(
        std::string_view name,
        std::string password = {},
        std::uint64_t epoch = 0);
    [[nodiscard]] MaintenanceResult merge_vault(
        std::string_view source_name,
        std::string password = {},
        std::uint64_t epoch = 0);
    [[nodiscard]] std::vector<std::string> list_r2_vaults(
        std::uint64_t epoch = 0) const;
    [[nodiscard]] cha::web::VaultDefinition download_r2_vault(
        std::string_view name,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::R2DatabaseTransfer upload_database();
    [[nodiscard]] cha::web::R2DatabaseTransfer download_database();
    [[nodiscard]] WorkspaceConfigTransfer import_configuration();
    [[nodiscard]] WorkspaceConfigTransfer export_configuration();
    void save_file(
        std::uint64_t epoch,
        const std::filesystem::path& destination,
        std::string_view contents);

    [[nodiscard]] std::optional<FullSessionId> selected_session() const;
    [[nodiscard]] std::uint64_t context_epoch() const;
    // Shares exclusion with maintenance: epoch check and resource use take
    // the same lifecycle mutex. A matching name is not enough.
    [[nodiscard]] std::optional<cha::web::ErrorCode> check_context(
        std::uint64_t epoch) const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] ApplicationState state() const;
    [[nodiscard]] ApplicationCapabilities capabilities() const;
    [[nodiscard]] bool has_r2_storage() const;
    void set_resource_hooks(ResourceHooks hooks);
    void set_context_changed(ContextChanged callback);

    void request_shutdown();
    [[nodiscard]] bool join_shutdown(
        std::chrono::milliseconds grace = std::chrono::milliseconds{10000});

    [[nodiscard]] cha::web::ApplicationCommand& command();
    [[nodiscard]] const cha::web::ApplicationCommand& command() const;
    [[nodiscard]] const cha::web::WebSettings& settings() const;
    [[nodiscard]] cha::web::CurrentVault& current_vault();
    [[nodiscard]] const cha::web::CurrentVault& current_vault() const;
    [[nodiscard]] cha::WorkspaceConfigStore& store();
    [[nodiscard]] std::shared_ptr<cha::SessionRepository> sessions();
    [[nodiscard]] cha::web::LiveSessionManager& live_sessions();
    [[nodiscard]] cha::Providers& providers();
    [[nodiscard]] cha::ApiKeyStore& api_keys();
    [[nodiscard]] cha::OpenAiOAuth& openai_auth();
    [[nodiscard]] std::shared_ptr<cha::web::SessionMirror> mirror();
    [[nodiscard]] std::string active_password() const;
    std::string& mutable_active_password();
    void set_active_password(std::string password);
    [[nodiscard]] std::timed_mutex& lifecycle_mutex();
    [[nodiscard]] bool unusable() const;
    bool& mutable_unusable();
    void mark_unusable();
    [[nodiscard]] bool stopping() const;

private:
    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cha::app
