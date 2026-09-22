#include "app/application_internal.h"

#include "app/settings_operations.h"
#include "app/vault_operations.h"
#include "app/workspace_operations.h"
#include "providers/openai_oauth.h"
#include "providers/api_key_store.h"
#include "providers/provider_client.h"
#include "providers/providers.h"
#include "storage/not_found_error.h"
#include "storage/session_label.h"
#include "storage/session_repository.h"
#include "util/logging.h"
#include "util/text.h"
#include "app/current_vault.h"
#include "session/session_markdown.h"
#include "session/session_mirror.h"
#include "runtime/session_projection.h"
#include "workspace/builtins.h"
#include "session/session_open.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cha::app {

ApplicationError::ApplicationError(ErrorCode code, std::string message)
    : std::runtime_error(
          message.empty() ? "The application operation failed" : std::move(message)),
      code(code) {}

bool OperationReply::complete(nlohmann::json result) {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex_);
        if (result_ || abandoned_) return false;
        result_ = std::move(result);
        callback = std::move(ready_callback_);
    }
    ready_.notify_all();
    if (callback) callback();
    return true;
}

bool OperationReply::fail(ErrorCode code, std::string message) {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex_);
        if (result_ || abandoned_) return false;
        result_ = Failure{code, std::move(message)};
        callback = std::move(ready_callback_);
    }
    ready_.notify_all();
    if (callback) callback();
    return true;
}

void OperationReply::set_ready_callback(std::function<void()> callback) {
    {
        std::lock_guard lock(mutex_);
        if (abandoned_) return;
        if (!result_) {
            ready_callback_ = std::move(callback);
            return;
        }
    }
    if (callback) callback();
}

std::optional<OperationReply::Result> OperationReply::peek() const {
    std::lock_guard lock(mutex_);
    return result_;
}

void OperationReply::abandon() const {
    std::lock_guard lock(mutex_);
    abandoned_ = true;
    ready_callback_ = {};
}

std::string_view application_state_name(ApplicationState state) noexcept {
    switch (state) {
    case ApplicationState::running:
        return "running";
    case ApplicationState::maintenance:
        return "maintenance";
    case ApplicationState::stopping:
        return "stopping";
    case ApplicationState::unavailable:
        return "unavailable";
    }
    return "unavailable";
}

namespace {

ProviderClientFactory shared_openai_provider_factory(
    OpenAiOAuth* oauth,
    ApiKeyStore* api_keys) {
    return [oauth, api_keys](SharedCharacterDefinition definition) {
        return std::make_unique<ProviderClient>(
            std::move(definition), oauth, api_keys);
    };
}

CharacterSummary character_summary(
    const Workspace& workspace,
    const WorkspaceCharacter& character) {
    return {
        .id = character.character.id,
        .display_name = character.character.display_name,
        .description = character.character.description,
        .appearance = character.character.appearance,
        .voice = resolve_speech_voice(workspace, character),
    };
}

PersonaSummary persona_summary(
    const Workspace& workspace,
    const WorkspacePersona& persona) {
    return {
        .id = persona.id,
        .display_name = persona.display_name,
        .description = persona.description,
        .appearance = persona.appearance,
        .voice = resolve_speech_voice(workspace, persona),
    };
}

ForumSummary forum_summary(
    const WorkspaceForum& forum,
    const Workspace& workspace) {
    const WorkspacePersona* persona =
        workspace.find_persona(forum.default_persona_id);
    if (persona == nullptr) {
        throw std::runtime_error(
            "Forum default persona is absent from the workspace");
    }
    ForumSummary result{
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
        [](const CharacterSummary& character) {
            return fold_ascii(character.display_name);
        });
    return result;
}

Bootstrap make_bootstrap(
    const Workspace& workspace,
    const std::vector<StoredSession>& recent,
    const FullSessionId& initial,
    std::string vault_name,
    std::vector<std::string> vaults) {
    Bootstrap bootstrap{
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

Application::Impl::Impl(
    const ApplicationCommand& selected_command,
    std::string selected_vault_password,
    RuntimeSettings selected_settings)
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
    vault_maintenance.publish_vault_names();
    const auto seed = TemporarySessionSeed{
        {std::string(entrance_id), std::string(welcome_id)},
        std::string(welcome_name)};
    sessions = std::make_shared<SessionRepository>(
        [this] { return store->snapshot(); },
        store->database_path(),
        store->workspace_path(),
        store->welcome_path(),
        seed,
        active_password);
    mirror = std::make_shared<SessionMirror>();
    if (const auto root = session_mirror_root(command.vault)) {
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
    live_sessions = std::make_unique<LiveSessionManager>(
        settings, opener);
    audio_downloads = std::make_unique<AudioDownloadManager>(
        *sessions, [this] { return current_vault_.get().name; }, true);
    publish_capabilities_locked();
    running = true;
    notified_epoch = live_sessions->context_epoch();
    published_epoch.store(notified_epoch);
    store->set_restart_required_handler([this] { mark_unusable(); });
}

void Application::Impl::mark_unusable() {
    unusable.store(true);
    state.store(ApplicationState::unavailable);
    live_sessions->begin_shutdown();
    pending_media.cancel_all();
    speech_proxy.stop();
    audio_downloads->request_stop();
}

void Application::Impl::publish_capabilities_locked() {
    unsigned capabilities = 0;
    if (current_vault_.get().modify) capabilities |= can_modify;
    if (api_keys->r2()) capabilities |= can_transfer_r2;
    published_capabilities.store(capabilities);
}

ApplicationCapabilities Application::Impl::capabilities() const noexcept {
    const auto epoch = published_epoch.load();
    if (state.load() != ApplicationState::running) return {};
    const unsigned capabilities = published_capabilities.load();
    if (state.load() != ApplicationState::running
        || published_epoch.load() != epoch) {
        return {};
    }
    return {
        .can_modify = (capabilities & can_modify) != 0,
        .can_transfer_r2 = (capabilities & can_transfer_r2) != 0,
    };
}

std::optional<ErrorCode> Application::Impl::admit_locked(std::uint64_t epoch) const {
    if (unusable || stopping_flag || stopped) {
        return ErrorCode::application_unavailable;
    }
    const ApplicationState current_state = state.load();
    if (current_state == ApplicationState::maintenance) {
        return ErrorCode::vault_changed;
    }
    if (current_state != ApplicationState::running) {
        return ErrorCode::application_unavailable;
    }
    if (epoch == 0 || epoch != published_epoch.load()) {
        return ErrorCode::vault_changed;
    }
    return std::nullopt;
}

void Application::Impl::require_admitted(std::uint64_t epoch) const {
    if (const auto error = admit_locked(epoch)) {
        throw ApplicationError(*error);
    }
}

void Application::Impl::pause_resources(bool cancel) {
    pending_media.cancel_all();
    media_resources.revoke_all();
    if (audio_downloads) audio_downloads->pause(cancel);
    if (resource_hooks.pause) resource_hooks.pause(cancel);
}

void Application::Impl::resume_resources() {
    if (unusable) return;
    if (audio_downloads) audio_downloads->resume();
    if (resource_hooks.resume) resource_hooks.resume();
}

void Application::Impl::take_context_notice(PendingContextNotice& notice) {
    const auto epoch = published_epoch.load();
    const ApplicationState current_state = state.load();
    if (notified_epoch == epoch && notified_state == current_state) return;
    notified_epoch = epoch;
    notified_state = current_state;
    notice = {context_changed, epoch, current_state};
}

Application::Impl::~Impl() {
    speech_proxy.stop();
    pending_media.cancel_all();
    if (audio_downloads) audio_downloads->request_stop();
    background_jobs.join();
    if (running && !stopped) {
        live_sessions->begin_shutdown();
        (void)live_sessions->join_shutdown(settings.shutdown_grace);
    }
    providers.shutdown();
}

Application::Application(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Application::~Application() {
    if (impl_ && impl_->preserve_on_destroy.load()) {
        // A worker that ignored cancellation still owns pointers into Impl.
        // The process is already stopping, so retaining Impl is safer than
        // either blocking destruction or detaching it from its dependencies.
        (void)impl_.release();
    }
}

std::unique_ptr<Application> Application::open(
    const ApplicationCommand& command,
    std::string vault_password,
    RuntimeSettings settings) {
    for (const std::string& warning : command.warnings) log_warn(warning);
    if (command.vault.password_protected && vault_password.empty()) {
        throw VaultPasswordError(
            "Password required to open this vault");
    }
    if (command.vault.password_protected) {
        require_openable_protected_database(
            command.vault.data, vault_password);
    }
    return std::unique_ptr<Application>(new Application(
        std::make_unique<Impl>(
            command, std::move(vault_password), std::move(settings))));
}

ApplicationBootstrap Application::bootstrap() {
    ApplicationBootstrap result;
    const auto read_published_state = [&] {
        const ApplicationState state = impl_->state.load();
        return state == ApplicationState::stopping
            ? ApplicationState::unavailable
            : state;
    };
    result.state = read_published_state();
    result.capabilities = impl_->capabilities();
    if (result.state != ApplicationState::running) {
        result.context_epoch = impl_->published_epoch.load();
        auto [vault, names] = impl_->current_vault_.snapshot();
        result.presentation.vault_name = std::move(vault.name);
        result.presentation.vaults = std::move(names);
        return result;
    }

    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    result.context_epoch = impl_->published_epoch.load();
    result.state = read_published_state();
    result.capabilities = impl_->capabilities();
    auto [vault, names] = impl_->current_vault_.snapshot();
    if (names.empty()) {
        for (const auto& definition : impl_->command.vaults) {
            names.push_back(definition.name);
        }
    }
    result.presentation.vault_name = vault.name;
    result.presentation.vaults = names;
    if (result.state != ApplicationState::running) return result;
    const auto workspace = impl_->store->snapshot();
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

CreateSessionSuccess Application::create_session(
    std::string_view forum_id,
    std::string label,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        if (!label.empty()) validate_session_label(label);
        const StoredSession created =
            impl_->sessions->create(forum_id, std::move(label));
        if (impl_->mirror) impl_->mirror->add(created);
        return {created.identity.session_id, created.label};
    } catch (const std::invalid_argument&) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "Invalid session label.");
    }
}

std::variant<OpenSessionSuccess, ErrorCode>
Application::open_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        if (const auto error = impl_->admit_locked(epoch)) return *error;
        try {
            impl_->sessions->validate(key);
        } catch (const SessionNotFoundError&) {
            return ErrorCode::not_found;
        } catch (const ForumNotFoundError&) {
            return ErrorCode::not_found;
        }
    }
    const auto outcome =
        impl_->live_sessions->select(key, impl_->settings.open_deadline);
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        if (const auto error = impl_->admit_locked(epoch)) {
            if (std::holds_alternative<LiveSessionReady>(outcome)) {
                impl_->live_sessions->close_session(key);
            }
            return *error;
        }
    }
    if (std::holds_alternative<LiveSessionReady>(outcome)) {
        return OpenSessionSuccess{key.forum_id, key.session_id};
    }
    switch (std::get<LiveSessionOpenFailure>(outcome)) {
    case LiveSessionOpenFailure::not_found:
        return ErrorCode::not_found;
    case LiveSessionOpenFailure::stopping:
        return ErrorCode::session_stopping;
    case LiveSessionOpenFailure::limit_reached:
        return ErrorCode::session_limit_reached;
    case LiveSessionOpenFailure::open_timeout:
        return ErrorCode::session_open_timeout;
    case LiveSessionOpenFailure::manager_stopping:
        return ErrorCode::application_unavailable;
    case LiveSessionOpenFailure::internal_error:
        return ErrorCode::internal_error;
    }
    return ErrorCode::internal_error;
}

CommandSubmitResult Application::submit(
    std::string_view forum_id,
    std::string_view session_id,
    WebCommand command,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    std::optional<ErrorCode> denied;
    LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->submit(std::move(command), impl_->settings.command_deadline);
}

std::variant<std::shared_ptr<CommandReply>, ErrorCode>
Application::submit_async(
    std::string_view forum_id,
    std::string_view session_id,
    WebCommand command,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    if (const auto denied = check_context(epoch)) return *denied;
    // The manager checks the epoch and maintenance gate with the lookup.
    // Stop must not wait for a transfer holding the application lifecycle lock.
    auto session = impl_->live_sessions->lookup(key, epoch);
    if (const auto denied = check_context(epoch)) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->enqueue(std::move(command));
}

CommandSubmitResult Application::stop(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    return submit(forum_id, session_id, StopCommand{}, epoch);
}

CommandSubmitResult Application::snapshot(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    std::optional<ErrorCode> denied;
    LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->snapshot(impl_->settings.command_deadline);
}

CommandSubmitResult Application::subscribe(
    std::string_view forum_id,
    std::string_view session_id,
    SubscribeCommand command,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    std::optional<ErrorCode> denied;
    LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->subscribe(std::move(command), impl_->settings.command_deadline);
}

CommandSubmitResult Application::unsubscribe(
    std::string_view forum_id,
    std::string_view session_id,
    UnsubscribeCommand command,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    std::optional<ErrorCode> denied;
    LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->unsubscribe(std::move(command), impl_->settings.command_deadline);
}

void Application::close_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    if (check_context(epoch)) return;
    impl_->live_sessions->close_session(
        {std::string(forum_id), std::string(session_id)}, epoch);
}

std::optional<ErrorCode> Application::delete_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    if (const auto error = impl_->admit_locked(epoch)) return *error;
    if (workspace::is_welcome_session(key.forum_id, key.session_id)) {
        return ErrorCode::not_found;
    }
    MaintenanceReservationResult reserved =
        impl_->live_sessions->reserve_for_deletion(
            key, impl_->settings.delete_deadline);
    if (const auto* failure =
            std::get_if<MaintenanceFailure>(&reserved)) {
        return *failure == MaintenanceFailure::manager_stopping
            ? ErrorCode::application_unavailable
            : ErrorCode::session_stopping;
    }
    try {
        impl_->sessions->delete_session(key);
    } catch (const SessionNotFoundError&) {
        return ErrorCode::not_found;
    } catch (const ForumNotFoundError&) {
        return ErrorCode::not_found;
    }
    impl_->media_resources.revoke_session(key);
    return std::nullopt;
}

std::vector<SessionListing> Application::list_sessions(
    std::string_view forum_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return workspace::sessions_for(
            *impl_->sessions, impl_->live_sessions->snapshot(), forum_id);
    } catch (const ForumNotFoundError&) {
        throw ApplicationError(ErrorCode::not_found);
    }
}

SessionLabelResult Application::rename_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::string label,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (workspace::is_welcome_session(key.forum_id, key.session_id)) {
        throw ApplicationError(ErrorCode::not_found);
    }
    try {
        validate_session_label(label);
    } catch (const std::invalid_argument&) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "Invalid session label.");
    }
    const LiveSessionHandle live = impl_->live_sessions->lookup(key);
    if (live) {
        const auto result = live->submit(
            RenameSessionCommand{std::move(label)},
            impl_->settings.command_deadline);
        if (const auto* renamed =
                std::get_if<SessionLabelResult>(&result)) {
            return *renamed;
        }
        if (const auto* error = std::get_if<ErrorCode>(&result)) {
            throw ApplicationError(*error);
        }
        throw ApplicationError(ErrorCode::internal_error);
    }
    try {
        const StoredSession renamed =
            impl_->sessions->rename(key, std::move(label));
        if (impl_->mirror) {
            impl_->mirror->update(
                renamed.identity,
                renamed.label,
                impl_->sessions->history(renamed.identity));
        }
        return {renamed.identity.session_id, renamed.label};
    } catch (const std::invalid_argument&) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "Invalid session label.");
    }
}

SessionExport Application::export_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const LiveSessionHandle live = impl_->live_sessions->lookup(key);
    if (live) {
        const auto result = live->snapshot(impl_->settings.command_deadline);
        if (const auto* snapshot =
                std::get_if<SessionSnapshot>(&result)) {
            return {session_markdown(
                snapshot->session_label, snapshot->transcript)};
        }
        if (const auto* error = std::get_if<ErrorCode>(&result)) {
            throw ApplicationError(*error);
        }
        throw ApplicationError(ErrorCode::internal_error);
    }
    try {
        const PreparedSession prepared = impl_->sessions->prepare(key);
        return {session_markdown(
            prepared.label, prepared.restore.entries)};
    } catch (const SessionNotFoundError&) {
        throw ApplicationError(ErrorCode::not_found);
    } catch (const ForumNotFoundError&) {
        throw ApplicationError(ErrorCode::not_found);
    }
}

CharacterDetail Application::get_character(
    std::string_view character_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_character(*impl_->store->snapshot(), character_id);
}

CharacterDetail Application::create_character(
    CreateCharacterRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_character(
        *impl_->store, create.display_name, create.description);
}

CharacterDetail Application::update_character(
    std::string_view character_id,
    CharacterSettingsUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_character_settings(
        *impl_->store, *impl_->live_sessions, character_id, update);
}

CharacterDetail Application::update_character_definition(
    std::string_view character_id,
    CharacterDefinitionUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_character_definition(
        *impl_->store, *impl_->live_sessions, character_id, update);
}

void Application::delete_character(
    std::string_view character_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_character(*impl_->store, character_id);
}

MarkdownFile Application::get_character_file(
    std::string_view character_id,
    std::string_view filename,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_character_file(
        *impl_->store->snapshot(), character_id, filename);
}

MarkdownFile Application::create_character_file(
    std::string_view character_id,
    std::string filename,
    std::string content,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_character_file(
        *impl_->store, *impl_->live_sessions, character_id,
        std::move(filename), std::move(content));
}

MarkdownFile Application::update_character_file(
    std::string_view character_id,
    std::string filename,
    std::string content,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_character_file(
        *impl_->store, *impl_->live_sessions, character_id,
        std::move(filename), std::move(content));
}

void Application::delete_character_file(
    std::string_view character_id,
    std::string_view filename,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_character_file(
        *impl_->store, *impl_->live_sessions, character_id, std::string(filename));
}

PersonaDetail Application::get_persona(
    std::string_view persona_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_persona(*impl_->store->snapshot(), persona_id);
}

PersonaDetail Application::create_persona(
    std::string display_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_persona(*impl_->store, display_name);
}

PersonaDetail Application::update_persona(
    std::string_view persona_id,
    PersonaUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_persona(
        *impl_->store, *impl_->live_sessions, persona_id, update);
}

void Application::delete_persona(
    std::string_view persona_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_persona(*impl_->store, persona_id);
}

ForumDetail Application::get_forum(
    std::string_view forum_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_forum(*impl_->store->snapshot(), forum_id);
}

ForumDetail Application::create_forum(
    CreateForumRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_forum(
        *impl_->store, create.display_name, create.persona_id);
}

ForumDetail Application::update_forum(
    std::string_view forum_id,
    ForumUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_forum(
        *impl_->store, *impl_->live_sessions, forum_id, update);
}

void Application::delete_forum(
    std::string_view forum_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_forum(*impl_->store, *impl_->live_sessions, forum_id);
}

ForumDetail Application::update_forum_members(
    std::string_view forum_id,
    ForumMembersUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_forum_members(
        *impl_->store, *impl_->live_sessions, forum_id, update);
}

MarkdownFile Application::get_forum_file(
    std::string_view forum_id,
    std::string_view filename,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_forum_file(*impl_->store->snapshot(), forum_id, filename);
}

MarkdownFile Application::create_forum_file(
    std::string_view forum_id,
    std::string filename,
    std::string content,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_forum_file(
        *impl_->store, *impl_->live_sessions, forum_id,
        std::move(filename), std::move(content));
}

MarkdownFile Application::update_forum_file(
    std::string_view forum_id,
    std::string filename,
    std::string content,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_forum_file(
        *impl_->store, *impl_->live_sessions, forum_id,
        std::move(filename), std::move(content));
}

void Application::delete_forum_file(
    std::string_view forum_id,
    std::string_view filename,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_forum_file(
        *impl_->store, *impl_->live_sessions, forum_id, std::string(filename));
}

std::vector<ProviderSummary> Application::list_providers(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::list_providers(*impl_->store->snapshot());
}

ProviderDetail Application::get_provider(
    std::string_view provider_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_provider(
        *impl_->store->snapshot(), provider_id, *impl_->api_keys);
}

ProviderDetail Application::create_provider(
    CreateProviderRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::create_provider(*impl_->store, *impl_->api_keys, create);
}

ProviderDetail Application::update_provider(
    std::string_view provider_id,
    nlohmann::json body,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::update_provider(
        *impl_->store,
        *impl_->live_sessions,
        *impl_->api_keys,
        provider_id,
        body);
}

void Application::delete_provider(
    std::string_view provider_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_provider(*impl_->store, provider_id);
}

std::shared_ptr<OperationReply> Application::test_provider(
    std::string_view provider_id,
    nlohmann::json body,
    std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    OpenAiOAuth* oauth = nullptr;
    ApiKeyStore* keys = nullptr;
    std::shared_ptr<const Workspace> workspace;
    std::string id(provider_id);
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        workspace = impl_->store->snapshot();
        (void)settings::get_provider(*workspace, id, *impl_->api_keys);
        oauth = impl_->openai_auth.get();
        keys = impl_->api_keys.get();
    }
    if (!impl_->background_jobs.launch(
            [reply, id = std::move(id), body = std::move(body), oauth, keys,
             workspace = std::move(workspace)](
                std::atomic_bool& cancel) {
                try {
                    settings::test_provider(
                        *workspace, id, body, *oauth, *keys, cancel);
                    if (cancel.load()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    reply->complete(nlohmann::json::object());
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (const std::exception& error) {
                    reply->fail(
                        ErrorCode::invalid_argument,
                        "Provider test failed: " + std::string(error.what()));
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

std::vector<StyleDetail> Application::list_styles(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::list_styles(*impl_->store->snapshot());
}

StyleDetail Application::create_style(
    std::string display_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::create_style(*impl_->store, display_name);
}

StyleDetail Application::update_style(
    std::string_view style_id,
    StyleUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::update_style(
        *impl_->store, *impl_->live_sessions, style_id, update);
}

void Application::delete_style(std::string_view style_id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_style(*impl_->store, style_id);
}

std::vector<VoiceDetail> Application::list_voices(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::list_voices(*impl_->store->snapshot());
}

VoiceDetail Application::create_voice(
    CreateVoiceRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::create_voice(*impl_->store, create);
}

VoiceDetail Application::update_voice(
    std::string_view voice_id,
    VoiceUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::update_voice(
        *impl_->store, *impl_->live_sessions, voice_id, update);
}

void Application::delete_voice(std::string_view voice_id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_voice(*impl_->store, voice_id);
}

std::optional<VoiceInputSettings>
Application::get_voice_input_settings(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_voice_input_settings(*impl_->store->snapshot());
}

VoiceInputSettings Application::save_voice_input_settings(
    VoiceInputSettings voice_settings,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::save_voice_input_settings(
        *impl_->store, *impl_->api_keys, voice_settings);
}

std::optional<VoiceInputRuntime>
Application::get_voice_input_runtime(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_voice_input_runtime(
        *impl_->store->snapshot(), *impl_->api_keys, true);
}

std::optional<VoiceOutputSettings>
Application::get_voice_output_settings(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_voice_output_settings(*impl_->store->snapshot());
}

VoiceOutputSettings Application::save_voice_output_settings(
    VoiceOutputSettings voice_settings,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::save_voice_output_settings(
        *impl_->store, *impl_->api_keys, voice_settings);
}

std::optional<VoiceOutputRuntime>
Application::get_voice_output_runtime(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_voice_output_runtime(
        *impl_->store->snapshot(), *impl_->api_keys, true);
}

std::vector<ApiKeyDetail> Application::list_api_keys(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::list_api_keys(*impl_->store->snapshot(), *impl_->api_keys);
}

ApiKeyDetail Application::create_api_key(
    CreateApiKeyRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::create_api_key(
        *impl_->store->snapshot(), *impl_->api_keys, create);
}

ApiKeyDetail Application::rename_api_key(
    std::string_view api_key_id,
    std::string display_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::rename_api_key(
        *impl_->store->snapshot(), *impl_->api_keys, api_key_id, display_name);
}

ApiKeyDetail Application::replace_api_key_value(
    std::string_view api_key_id,
    std::string value,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::replace_api_key_value(
        *impl_->store->snapshot(), *impl_->api_keys, api_key_id, value);
}

void Application::delete_api_key(
    std::string_view api_key_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_api_key(*impl_->api_keys, api_key_id);
}

std::optional<R2StorageDetail> Application::get_r2_storage(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_r2_storage(*impl_->api_keys);
}

R2StorageDetail Application::save_r2_storage(
    SaveR2StorageRequest request,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    auto saved = settings::save_r2_storage(*impl_->api_keys, request);
    impl_->publish_capabilities_locked();
    return saved;
}

void Application::delete_r2_storage(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_r2_storage(*impl_->api_keys);
    impl_->publish_capabilities_locked();
}

OpenAiAuth Application::openai_auth_status(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::openai_auth_status(*impl_->openai_auth);
}

std::shared_ptr<OperationReply> Application::start_openai_auth(
    std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    OpenAiOAuth* oauth = nullptr;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        oauth = impl_->openai_auth.get();
    }
    if (!impl_->background_jobs.launch(
            [reply, oauth](std::atomic_bool& cancel) {
                try {
                    const auto result =
                        settings::start_openai_auth(*oauth, cancel);
                    if (cancel.load()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    reply->complete(result);
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

std::shared_ptr<OperationReply> Application::poll_openai_auth(
    std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    OpenAiOAuth* oauth = nullptr;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        oauth = impl_->openai_auth.get();
    }
    if (!impl_->background_jobs.launch(
            [reply, oauth](std::atomic_bool& cancel) {
                try {
                    const auto result =
                        settings::poll_openai_auth(*oauth, cancel);
                    if (cancel.load()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    reply->complete(result);
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

OpenAiAuth Application::disconnect_openai_auth(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::disconnect_openai_auth(*impl_->openai_auth);
}

std::optional<FullSessionId> Application::selected_session() const {
    return impl_->live_sessions->selected();
}

std::uint64_t Application::context_epoch() const {
    return impl_->published_epoch.load();
}

std::optional<ErrorCode> Application::check_context(
    std::uint64_t epoch) const {
    const ApplicationState state = impl_->state.load();
    if (state == ApplicationState::maintenance) {
        return ErrorCode::vault_changed;
    }
    if (state != ApplicationState::running) {
        return ErrorCode::application_unavailable;
    }
    if (epoch == 0 || epoch != impl_->published_epoch.load()) {
        return ErrorCode::vault_changed;
    }
    return std::nullopt;
}

bool Application::running() const {
    return impl_->state.load() == ApplicationState::running;
}

ApplicationState Application::state() const {
    const ApplicationState state = impl_->state.load();
    if (state == ApplicationState::stopping) {
        return ApplicationState::unavailable;
    }
    return state;
}

ApplicationCapabilities Application::capabilities() const {
    return impl_->capabilities();
}

bool Application::has_r2_storage() const {
    return impl_->capabilities().can_transfer_r2;
}

void Application::set_resource_hooks(ResourceHooks hooks) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->resource_hooks = std::move(hooks);
}

void Application::set_context_changed(ContextChanged callback) {
    Impl::PendingContextNotice notice;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->context_changed = std::move(callback);
        impl_->take_context_notice(notice);
    }
    notice.dispatch();
}

void Application::request_shutdown() {
    impl_->stopping_flag = true;
    impl_->state.store(ApplicationState::stopping);
    impl_->speech_proxy.stop();
    if (impl_->audio_downloads) impl_->audio_downloads->request_stop();
    impl_->pause_resources(true);
    impl_->live_sessions->begin_shutdown();
}

void Application::wait_for_maintenance() const {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
}

bool Application::join_shutdown(std::chrono::milliseconds grace) {
    const auto deadline = std::chrono::steady_clock::now() + grace;
    if (!impl_->background_jobs.join_until(deadline)) {
        impl_->state.store(ApplicationState::unavailable);
        impl_->preserve_on_destroy.store(true);
        return false;
    }
    std::unique_lock lifecycle(impl_->lifecycle_mutex, std::defer_lock);
    if (!lifecycle.try_lock_until(deadline)) {
        impl_->state.store(ApplicationState::unavailable);
        impl_->preserve_on_destroy.store(true);
        return false;
    }
    bool joined = true;
    if (impl_->audio_downloads) {
        impl_->audio_downloads->request_stop();
        joined = impl_->audio_downloads->join_until(deadline) && joined;
    }
    impl_->media_resources.revoke_all();
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = now < deadline
        ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
        : std::chrono::milliseconds::zero();
    joined = impl_->live_sessions->join_shutdown(remaining) && joined;
    joined = impl_->providers.shutdown_until(deadline) && joined;
    impl_->stopped = true;
    impl_->running = false;
    impl_->state.store(ApplicationState::unavailable);
    impl_->preserve_on_destroy.store(!joined);
    return joined;
}

VaultRegistrySnapshot Application::vault_snapshot() const {
    return impl_->vault_maintenance.vault_snapshot();
}

VaultDefinition Application::create_vault(VaultCreate create, std::uint64_t epoch) {
    return impl_->vault_maintenance.create_vault(std::move(create), epoch);
}

VaultDefinition Application::update_vault(
    std::string_view current_name,
    VaultUpdate update,
    std::uint64_t epoch) {
    return impl_->vault_maintenance.update_vault(
        current_name, std::move(update), epoch);
}

void Application::delete_vault(std::string_view name, std::uint64_t epoch) {
    impl_->vault_maintenance.delete_vault(name, epoch);
}

std::vector<std::string> Application::list_r2_vaults(std::uint64_t epoch) const {
    return impl_->vault_maintenance.list_r2_vaults(epoch);
}

VaultDefinition Application::download_r2_vault(
    std::string_view name,
    std::uint64_t epoch) {
    return impl_->vault_maintenance.download_r2_vault(name, epoch);
}

MaintenanceResult Application::switch_vault(
    std::string_view name,
    std::string password,
    std::uint64_t epoch) {
    return impl_->vault_maintenance.switch_vault(name, std::move(password), epoch);
}

MaintenanceResult Application::merge_vault(
    std::string_view source_name,
    std::string password,
    std::uint64_t epoch) {
    return impl_->vault_maintenance.merge_vault(
        source_name, std::move(password), epoch);
}

R2DatabaseTransfer Application::upload_database(std::uint64_t epoch) {
    return impl_->vault_maintenance.upload_database(epoch);
}

R2DatabaseTransfer Application::download_database(std::uint64_t epoch) {
    return impl_->vault_maintenance.download_database(epoch);
}

WorkspaceConfigTransfer Application::import_configuration(std::uint64_t epoch) {
    return impl_->vault_maintenance.import_configuration(epoch);
}

WorkspaceConfigTransfer Application::export_configuration(std::uint64_t epoch) {
    return impl_->vault_maintenance.export_configuration(epoch);
}

void Application::save_file(
    std::uint64_t epoch,
    const std::filesystem::path& destination,
    std::string_view contents) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    vault::save_file_replace(destination, contents);
}

const RuntimeSettings& Application::settings() const {
    return impl_->settings;
}

LiveSessionHandle Application::subscription_handle(
    std::string_view forum_id, std::string_view session_id) {
    return impl_->live_sessions->lookup(
        {std::string(forum_id), std::string(session_id)});
}

std::size_t Application::live_session_count() const {
    return impl_->live_sessions->snapshot().live_session_count;
}

CurrentVault& Application::current_vault() {
    return impl_->current_vault_;
}

const CurrentVault& Application::current_vault() const {
    return impl_->current_vault_;
}

WorkspaceConfigStore& Application::store() {
    return *impl_->store;
}

ApiKeyStore& Application::api_keys() {
    return *impl_->api_keys;
}

std::string Application::active_password() const {
    return impl_->active_password;
}

void Application::mark_unusable() {
    impl_->mark_unusable();
}

} // namespace cha::app
