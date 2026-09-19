#include "app/application.h"

#include "providers/openai_oauth.h"
#include "providers/api_key_store.h"
#include "providers/provider_client.h"
#include "providers/providers.h"
#include "session/not_found_error.h"
#include "session/session_repository.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/text.h"
#include "web/current_vault.h"
#include "web/session_mirror.h"
#include "web/session_projection.h"
#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <algorithm>
#include <atomic>
#include <span>
#include <stdexcept>
#include <utility>

using cha::web::WebSettings;

namespace cha::app {
namespace {

ProviderClientFactory shared_openai_provider_factory(
    OpenAiOAuth* oauth,
    ApiKeyStore* api_keys) {
    return [oauth, api_keys](SharedCharacterDefinition definition) {
        return std::make_unique<ProviderClient>(
            std::move(definition), oauth, api_keys);
    };
}

std::shared_ptr<const Workspace> current_workspace() {
    std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    return workspace;
}

cha::web::CharacterSummary character_summary(
    const Workspace& workspace,
    const WorkspaceCharacter& character) {
    return {
        .id = character.character.id,
        .display_name = character.character.display_name,
        .description = character.character.description,
        .appearance = character.character.appearance,
        .voice = cha::web::resolve_speech_voice(workspace, character),
    };
}

cha::web::PersonaSummary persona_summary(
    const Workspace& workspace,
    const WorkspacePersona& persona) {
    return {
        .id = persona.id,
        .display_name = persona.display_name,
        .description = persona.description,
        .appearance = persona.appearance,
        .voice = cha::web::resolve_speech_voice(workspace, persona),
    };
}

cha::web::ForumSummary forum_summary(
    const WorkspaceForum& forum,
    const Workspace& workspace) {
    const WorkspacePersona* persona =
        workspace.find_persona(forum.default_persona_id);
    if (persona == nullptr) {
        throw std::runtime_error(
            "Forum default persona is absent from the workspace");
    }
    cha::web::ForumSummary result{
        .id = forum.id,
        .display_name = forum.display_name,
        .description = forum.description,
        .default_character_id = forum.default_character_id,
        .default_persona_id = forum.default_persona_id,
        .default_persona_display_name = persona->display_name,
    };
    result.members.reserve(forum.members.size());
    for (const WorkspaceForumMember& member : forum.members) {
        const WorkspaceCharacter* character =
            workspace.find_character(member.character_id);
        if (character == nullptr) {
            throw std::runtime_error(
                "Forum member is absent from the workspace");
        }
        result.members.push_back(character_summary(workspace, *character));
    }
    std::ranges::sort(
        result.members, {},
        [](const cha::web::CharacterSummary& character) {
            return fold_ascii(character.display_name);
        });
    return result;
}

cha::web::Bootstrap make_bootstrap(
    const Workspace& workspace,
    const std::vector<StoredSession>& recent,
    const FullSessionId& initial,
    std::string vault_name,
    std::vector<std::string> vaults) {
    cha::web::Bootstrap bootstrap{
        .initial_forum_id = initial.forum_id,
        .initial_session_id = initial.session_id};
    for (const WorkspacePersona& persona : workspace.personas()) {
        bootstrap.personas.push_back(persona_summary(workspace, persona));
    }
    for (const WorkspaceCharacter& character : workspace.characters()) {
        bootstrap.characters.push_back(character_summary(workspace, character));
    }
    for (const WorkspaceForum& forum : workspace.forums()) {
        bootstrap.forums.push_back(forum_summary(forum, workspace));
    }
    bootstrap.recent_sessions.reserve(recent.size());
    for (const StoredSession& stored : recent) {
        bootstrap.recent_sessions.push_back({
            stored.identity.forum_id,
            stored.identity.session_id,
            stored.label,
            stored.updated_at});
    }
    bootstrap.vault_name = std::move(vault_name);
    bootstrap.vaults = std::move(vaults);
    return bootstrap;
}

FullSessionId fallback_session(const SessionRepository& sessions) {
    const FullSessionId welcome{
        std::string(entrance_id), std::string(welcome_id)};
    try {
        sessions.validate(welcome);
        return welcome;
    } catch (const std::exception&) {
    }
    const auto recent = sessions.recent();
    if (!recent.empty()) return recent.front().identity;
    return welcome;
}

} // namespace

cha::web::WebSettings native_settings() {
    WebSettings settings;
    settings.browser_disconnect_lifetime = false;
    settings.monotonic_event_sequence = true;
    return settings;
}

struct Application::Impl {
    explicit Impl(
        const cha::web::ApplicationCommand& selected_command,
        std::string selected_vault_password,
        WebSettings selected_settings)
        : command(selected_command),
          active_password(std::move(selected_vault_password)),
          settings(std::move(selected_settings)),
          current_vault_(selected_command.vault),
          store(WorkspaceConfigStore::open(
              command.vault.data, active_password)),
          api_keys(std::make_unique<ApiKeyStore>(
              *store,
              command.config_directory / "api-keys.json")),
          openai_auth(std::make_unique<OpenAiOAuth>(
              command.config_directory / "openai-auth.json")),
          providers(shared_openai_provider_factory(
              openai_auth.get(), api_keys.get())) {
        publish_vault_names();
        const auto seed = TemporarySessionSeed{
            {std::string(entrance_id), std::string(welcome_id)},
            std::string(welcome_name)};
        sessions = std::make_shared<SessionRepository>(
            store->database_path(),
            store->workspace_path(),
            store->welcome_path(),
            seed,
            active_password);
        mirror = std::make_shared<cha::web::SessionMirror>();
        if (const auto root = cha::web::session_mirror_root(command.vault)) {
            try {
                mirror->rebuild(root, *sessions);
            } catch (const std::exception& error) {
                log_warn(
                    "Session mirror rebuild failed: "
                    + std::string(error.what()));
                mirror->rebuild(std::nullopt, *sessions);
            }
        }

        auto opener = [this](
                          const FullSessionId& identity,
                          std::shared_ptr<WakeNotifier> notifier) {
            OpenedSession opened = cha::open_session(
                *sessions, identity, providers, std::move(notifier), *store);
            const auto selected_mirror = mirror;
            opened.mirror = [selected_mirror, identity](
                                std::string_view label,
                                std::span<const TranscriptEntry> entries) {
                selected_mirror->update(identity, label, entries);
            };
            return opened;
        };
        live_sessions = std::make_unique<cha::web::LiveSessionManager>(
            settings, opener);
        running = true;
    }

    void publish_vault_names() {
        std::vector<std::string> names;
        names.reserve(command.vaults.size());
        for (const cha::web::VaultDefinition& vault : command.vaults) {
            names.push_back(vault.name);
        }
        current_vault_.set_names(std::move(names));
    }

    ~Impl() {
        if (running && !stopped) {
            live_sessions->begin_shutdown();
            (void)live_sessions->join_shutdown(settings.shutdown_grace);
        }
        providers.shutdown();
    }

    cha::web::ApplicationCommand command;
    std::string active_password;
    WebSettings settings;
    cha::web::CurrentVault current_vault_;
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<SessionRepository> sessions;
    std::shared_ptr<cha::web::SessionMirror> mirror;
    std::unique_ptr<ApiKeyStore> api_keys;
    std::unique_ptr<OpenAiOAuth> openai_auth;
    Providers providers;
    std::unique_ptr<cha::web::LiveSessionManager> live_sessions;
    mutable std::timed_mutex lifecycle_mutex;
    std::atomic_bool stopping_flag{};
    bool running{};
    bool stopped{};
    bool unusable{};
};

Application::Application(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Application::~Application() = default;

std::unique_ptr<Application> Application::open(
    const cha::web::ApplicationCommand& command,
    std::string vault_password,
    WebSettings settings) {
    for (const std::string& warning : command.warnings) log_warn(warning);
    load_dotenv(command.config_directory / ".env");
    if (command.vault.password_protected && vault_password.empty()) {
        throw cha::web::VaultPasswordError(
            "Password required to open this vault");
    }
    if (command.vault.password_protected) {
        cha::web::require_openable_protected_database(
            command.vault.data, vault_password);
    }
    return std::unique_ptr<Application>(new Application(
        std::make_unique<Impl>(
            command, std::move(vault_password), std::move(settings))));
}

ApplicationBootstrap Application::bootstrap() {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    ApplicationBootstrap result;
    result.context_epoch = impl_->live_sessions->context_epoch();
    if (impl_->unusable) {
        result.state = ApplicationState::unavailable;
        return result;
    }
    if (impl_->stopping_flag || impl_->stopped) {
        result.state = ApplicationState::unavailable;
        return result;
    }
    result.state = ApplicationState::ready;
    const auto workspace = current_workspace();
    auto [vault, names] = impl_->current_vault_.snapshot();
    if (names.empty()) {
        for (const auto& definition : impl_->command.vaults) {
            names.push_back(definition.name);
        }
    }
    FullSessionId initial = fallback_session(*impl_->sessions);
    if (const auto selected = impl_->live_sessions->selected()) {
        try {
            impl_->sessions->validate(*selected);
            initial = *selected;
        } catch (const std::exception&) {
            initial = fallback_session(*impl_->sessions);
        }
    }
    result.presentation = make_bootstrap(
        *workspace,
        impl_->sessions->recent(),
        initial,
        std::move(vault.name),
        std::move(names));
    return result;
}

cha::web::CreateSessionSuccess Application::create_session(
    std::string_view forum_id,
    std::string label) {
    if (impl_->stopping_flag || impl_->unusable || impl_->stopped) {
        throw std::runtime_error("CHA application is unavailable");
    }
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    if (impl_->stopping_flag || impl_->unusable || impl_->stopped) {
        throw std::runtime_error("CHA application is unavailable");
    }
    const StoredSession created =
        impl_->sessions->create(forum_id, std::move(label));
    if (impl_->mirror) impl_->mirror->add(created);
    return {created.identity.session_id, created.label};
}

std::variant<cha::web::OpenSessionSuccess, cha::web::ErrorCode>
Application::open_session(
    std::string_view forum_id,
    std::string_view session_id) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    if (impl_->stopping_flag || impl_->unusable) {
        return cha::web::ErrorCode::application_unavailable;
    }
    try {
        impl_->sessions->validate(key);
    } catch (const SessionNotFoundError&) {
        return cha::web::ErrorCode::not_found;
    } catch (const ForumNotFoundError&) {
        return cha::web::ErrorCode::not_found;
    }
    const auto outcome =
        impl_->live_sessions->select(key, impl_->settings.open_deadline);
    if (std::holds_alternative<cha::web::LiveSessionReady>(outcome)) {
        return cha::web::OpenSessionSuccess{key.forum_id, key.session_id};
    }
    switch (std::get<cha::web::LiveSessionOpenFailure>(outcome)) {
    case cha::web::LiveSessionOpenFailure::not_found:
        return cha::web::ErrorCode::not_found;
    case cha::web::LiveSessionOpenFailure::stopping:
        return cha::web::ErrorCode::session_stopping;
    case cha::web::LiveSessionOpenFailure::limit_reached:
        return cha::web::ErrorCode::session_limit_reached;
    case cha::web::LiveSessionOpenFailure::open_timeout:
        return cha::web::ErrorCode::session_open_timeout;
    case cha::web::LiveSessionOpenFailure::manager_stopping:
        return cha::web::ErrorCode::application_unavailable;
    case cha::web::LiveSessionOpenFailure::internal_error:
        return cha::web::ErrorCode::internal_error;
    }
    return cha::web::ErrorCode::internal_error;
}

cha::web::CommandSubmitResult Application::submit(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::WebCommand command) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    auto session = impl_->live_sessions->lookup(key);
    if (!session) return cha::web::ErrorCode::session_not_live;
    return session->submit(std::move(command), impl_->settings.command_deadline);
}

std::variant<std::shared_ptr<cha::web::CommandReply>, cha::web::ErrorCode>
Application::submit_async(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::WebCommand command) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    auto session = impl_->live_sessions->lookup(key);
    if (!session) return cha::web::ErrorCode::session_not_live;
    return session->enqueue(std::move(command));
}

cha::web::CommandSubmitResult Application::stop(
    std::string_view forum_id,
    std::string_view session_id) {
    return submit(forum_id, session_id, cha::web::StopCommand{});
}

cha::web::CommandSubmitResult Application::snapshot(
    std::string_view forum_id,
    std::string_view session_id) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    auto session = impl_->live_sessions->lookup(key);
    if (!session) return cha::web::ErrorCode::session_not_live;
    return session->snapshot(impl_->settings.command_deadline);
}

cha::web::CommandSubmitResult Application::subscribe(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::SubscribeCommand command) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    auto session = impl_->live_sessions->lookup(key);
    if (!session) return cha::web::ErrorCode::session_not_live;
    return session->subscribe(std::move(command), impl_->settings.command_deadline);
}

cha::web::CommandSubmitResult Application::unsubscribe(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::UnsubscribeCommand command) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    auto session = impl_->live_sessions->lookup(key);
    if (!session) return cha::web::ErrorCode::session_not_live;
    return session->unsubscribe(std::move(command), impl_->settings.command_deadline);
}

void Application::close_session(
    std::string_view forum_id,
    std::string_view session_id) {
    impl_->live_sessions->close_session(
        {std::string(forum_id), std::string(session_id)});
}

std::optional<cha::web::ErrorCode> Application::delete_session(
    std::string_view forum_id,
    std::string_view session_id) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    if (impl_->stopping_flag || impl_->unusable) {
        return cha::web::ErrorCode::application_unavailable;
    }
    cha::web::MaintenanceReservationResult reserved =
        impl_->live_sessions->reserve_for_deletion(
            key, impl_->settings.delete_deadline);
    if (const auto* failure =
            std::get_if<cha::web::MaintenanceFailure>(&reserved)) {
        return *failure == cha::web::MaintenanceFailure::manager_stopping
            ? cha::web::ErrorCode::application_unavailable
            : cha::web::ErrorCode::session_stopping;
    }
    try {
        impl_->sessions->delete_session(key);
    } catch (const SessionNotFoundError&) {
        return cha::web::ErrorCode::not_found;
    } catch (const ForumNotFoundError&) {
        return cha::web::ErrorCode::not_found;
    }
    return std::nullopt;
}

std::optional<FullSessionId> Application::selected_session() const {
    return impl_->live_sessions->selected();
}

std::uint64_t Application::context_epoch() const {
    return impl_->live_sessions->context_epoch();
}

std::optional<cha::web::ErrorCode> Application::check_context(
    std::uint64_t epoch) const {
    // Same mutex bootstrap/create use. Maintenance admission is later work;
    // this does not span blocking select()/open_deadline.
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    if (impl_->unusable || impl_->stopping_flag || impl_->stopped) {
        return cha::web::ErrorCode::application_unavailable;
    }
    if (epoch != impl_->live_sessions->context_epoch()) {
        return cha::web::ErrorCode::vault_changed;
    }
    return std::nullopt;
}

bool Application::running() const {
    return impl_->running && !impl_->stopped && !impl_->stopping_flag;
}

ApplicationState Application::state() const {
    if (impl_->unusable) return ApplicationState::unavailable;
    if (!running()) return ApplicationState::unavailable;
    return ApplicationState::ready;
}

void Application::request_shutdown() {
    impl_->stopping_flag = true;
    impl_->live_sessions->begin_shutdown();
}

bool Application::join_shutdown(std::chrono::milliseconds grace) {
    const bool joined = impl_->live_sessions->join_shutdown(grace);
    impl_->providers.shutdown();
    impl_->stopped = true;
    impl_->running = false;
    return joined;
}

cha::web::ApplicationCommand& Application::command() {
    return impl_->command;
}

const cha::web::ApplicationCommand& Application::command() const {
    return impl_->command;
}

const WebSettings& Application::settings() const {
    return impl_->settings;
}

cha::web::CurrentVault& Application::current_vault() {
    return impl_->current_vault_;
}

const cha::web::CurrentVault& Application::current_vault() const {
    return impl_->current_vault_;
}

WorkspaceConfigStore& Application::store() {
    return *impl_->store;
}

std::shared_ptr<SessionRepository> Application::sessions() {
    return impl_->sessions;
}

cha::web::LiveSessionManager& Application::live_sessions() {
    return *impl_->live_sessions;
}

Providers& Application::providers() {
    return impl_->providers;
}

ApiKeyStore& Application::api_keys() {
    return *impl_->api_keys;
}

OpenAiOAuth& Application::openai_auth() {
    return *impl_->openai_auth;
}

std::shared_ptr<cha::web::SessionMirror> Application::mirror() {
    return impl_->mirror;
}

std::string Application::active_password() const {
    return impl_->active_password;
}

std::string& Application::mutable_active_password() {
    return impl_->active_password;
}

void Application::set_active_password(std::string password) {
    impl_->active_password = std::move(password);
}

std::timed_mutex& Application::lifecycle_mutex() {
    return impl_->lifecycle_mutex;
}

bool Application::unusable() const {
    return impl_->unusable;
}

bool& Application::mutable_unusable() {
    return impl_->unusable;
}

void Application::mark_unusable() {
    impl_->unusable = true;
}

bool Application::stopping() const {
    return impl_->stopping_flag;
}

} // namespace cha::app
