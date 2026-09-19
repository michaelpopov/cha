#pragma once

#include "web/application_config.h"
#include "web/command_queue.h"
#include "web/live_session_manager.h"
#include "web/protocol.h"
#include "web/web_settings.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace cha {
class SessionRepository;
class WorkspaceConfigStore;
class Providers;
class ApiKeyStore;
class OpenAiOAuth;
}

namespace cha::web {
class CurrentVault;
class SessionMirror;
class AudioDownloadManager;
}

namespace cha::app {

enum class ApplicationState {
    ready,
    unavailable,
    maintenance,
};

struct ApplicationBootstrap {
    ApplicationState state{ApplicationState::ready};
    std::uint64_t context_epoch{1};
    cha::web::Bootstrap presentation;
};

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
        std::string label);
    [[nodiscard]] std::variant<
        cha::web::OpenSessionSuccess,
        cha::web::ErrorCode>
    open_session(
        std::string_view forum_id,
        std::string_view session_id);
    [[nodiscard]] cha::web::CommandSubmitResult submit(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::WebCommand command);
    [[nodiscard]] std::variant<
        std::shared_ptr<cha::web::CommandReply>,
        cha::web::ErrorCode>
    submit_async(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::WebCommand command);
    [[nodiscard]] cha::web::CommandSubmitResult stop(
        std::string_view forum_id,
        std::string_view session_id);
    [[nodiscard]] cha::web::CommandSubmitResult snapshot(
        std::string_view forum_id,
        std::string_view session_id);
    [[nodiscard]] cha::web::CommandSubmitResult subscribe(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::SubscribeCommand command);
    [[nodiscard]] cha::web::CommandSubmitResult unsubscribe(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::UnsubscribeCommand command);
    void close_session(std::string_view forum_id, std::string_view session_id);
    [[nodiscard]] std::optional<cha::web::ErrorCode> delete_session(
        std::string_view forum_id,
        std::string_view session_id);

    [[nodiscard]] std::optional<FullSessionId> selected_session() const;
    [[nodiscard]] std::uint64_t context_epoch() const;
    // Reads state and epoch under lifecycle_mutex, matching bootstrap/create.
    [[nodiscard]] std::optional<cha::web::ErrorCode> check_context(
        std::uint64_t epoch) const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] ApplicationState state() const;

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
