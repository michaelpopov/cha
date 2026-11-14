#include "app/vault_operations.h"

#include "app/application_internal.h"
#include "app/workspace_operations.h"
#include "storage/sqlite_storage.h"
#include "storage/sync_storage.h"
#include "storage/workspace_session_database.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "util/public_name.h"
#include "util/text.h"
#include "util/toml_file.h"
#include "workspace/builtins.h"
#include "workspace/prompt_catalog.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace cha::app::vault {
namespace {

bool is_workspace_directory(const std::filesystem::path& path) {
    try {
        (void)Workspace::load(path);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<std::filesystem::path> vault_path(
    const std::optional<std::filesystem::path>& base,
    std::string_view vault_name) {
    if (!base) return std::nullopt;
    return std::filesystem::weakly_canonical(
        *base / path_from_utf8(vault_name));
}

void write_vault_selection(
    const std::filesystem::path& config_directory,
    const std::string& vault_name) {
    rewrite_toml_file(
        config_directory / "app.toml",
        [&](toml::table& table) {
            table.insert_or_assign("vault", vault_name);
        });
}

std::string_view welcome_command_name(std::string_view input) {
    input = trim_view(input);
    const auto name = input.substr(0, find_whitespace(input));
    return name == "/download" || name == "/open" ? name : std::string_view{};
}

} // namespace

std::filesystem::path normalized_vault_path(
    const std::filesystem::path& config_directory,
    const std::filesystem::path& value) {
    if (value.empty()) throw std::invalid_argument("A vault path is empty");
    const std::filesystem::path resolved = value.is_relative()
        ? config_directory / value : value;
    return std::filesystem::weakly_canonical(
        std::filesystem::absolute(resolved));
}

toml::table vault_definition_table(const VaultDefinition& vault) {
    toml::table table;
    table.insert("vault_name", vault.name);
    table.insert("data", utf8_path(vault.data));
    table.insert("protected", vault.password_protected);
    if (vault.r2_etag) table.insert("r2_etag", *vault.r2_etag);
    if (vault.parent) table.insert("parent", *vault.parent);
    return table;
}

void assign_vault_paths(
    VaultDefinition& vault,
    const ApplicationCommand& command) {
    vault.mirror = vault_path(command.mirror_base, vault.name);
    vault.modify = vault_path(command.modify_base, vault.name);
}

std::filesystem::path next_vault_file(
    const std::filesystem::path& config_directory) {
    for (std::size_t suffix = 1;; ++suffix) {
        const std::filesystem::path candidate =
            config_directory / ("vault-" + std::to_string(suffix) + ".toml");
        if (!std::filesystem::exists(candidate)) return candidate;
    }
}

void validate_candidate_vaults(
    const std::filesystem::path& config_directory,
    const std::vector<VaultDefinition>& vaults) {
    try {
        for (const VaultDefinition& vault : vaults) {
            validate_public_name(vault.name, "vault_name", vault.source);
            require_path_component(vault.name, vault.source);
        }
        validate_vault_definitions(config_directory, vaults);
    } catch (const std::runtime_error& error) {
        throw std::invalid_argument(error.what());
    }
}

void validate_vault_paths(const VaultDefinition& vault) {
    if (!vault.password_protected && vault.mirror
        && std::filesystem::exists(*vault.mirror)
        && !std::filesystem::is_directory(*vault.mirror)) {
        throw std::invalid_argument(
            "The mirror path must be an existing directory");
    }
    if (!vault.modify || !std::filesystem::exists(*vault.modify)) return;
    if (!std::filesystem::is_directory(*vault.modify)) {
        throw std::invalid_argument(
            "The modify path must be a directory");
    }
    if (std::filesystem::is_empty(*vault.modify)) return;
    if (!is_workspace_directory(*vault.modify)) {
        throw std::invalid_argument(
            "The modify path must be empty or contain a valid CHA workspace");
    }
}

void require_available_database_path(const std::filesystem::path& data) {
    if (!std::filesystem::is_directory(data.parent_path())) {
        throw std::invalid_argument("The database parent directory does not exist");
    }
    std::error_code status_error;
    const std::filesystem::file_status data_status =
        std::filesystem::symlink_status(data, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("The database path could not be inspected");
    }
    if (data_status.type() != std::filesystem::file_type::not_found) {
        throw std::invalid_argument("The database path already exists");
    }
}

void clear_existing_export(const std::filesystem::path& destination) {
    std::filesystem::remove_all(destination);
}

void move_vault_directory(
    const std::optional<std::filesystem::path>& from,
    const std::optional<std::filesystem::path>& to,
    std::string_view field,
    std::vector<VaultDirectoryMove>& moved) {
    if (!from || !to || *from == *to || !std::filesystem::exists(*from)) return;
    if (std::filesystem::exists(*to)) {
        if (std::filesystem::equivalent(*from, *to)) return;
        throw std::invalid_argument(
            "The " + std::string(field)
            + " directory for the renamed vault already exists");
    }
    std::filesystem::rename(*from, *to);
    moved.emplace_back(*from, *to);
}

void restore_vault_directories(
    const std::vector<VaultDirectoryMove>& moved) noexcept {
    for (auto entry = moved.rbegin(); entry != moved.rend(); ++entry) {
        std::error_code error;
        std::filesystem::rename(entry->second, entry->first, error);
        if (error) {
            log_critical(
                "Failed to restore vault directory '"
                + utf8_path(entry->second) + "' to '"
                + utf8_path(entry->first) + "': " + error.message());
        }
    }
}

nlohmann::json vault_detail_json(
    const VaultDefinition& vault,
    std::string_view active_name,
    std::size_t vault_count) {
    return {
        {"display_name", vault.name},
        {"protected", vault.password_protected},
        {"data_path", utf8_path(vault.data)},
        {"mirror_path", vault.mirror
            ? nlohmann::json(utf8_path(*vault.mirror)) : nlohmann::json(nullptr)},
        {"modify_path", vault.modify
            ? nlohmann::json(utf8_path(*vault.modify)) : nlohmann::json(nullptr)},
        {"active", same_vault_name(vault.name, active_name)},
        {"can_delete", vault_count > 1
            && !same_vault_name(vault.name, active_name)},
    };
}

void save_file_replace(
    const std::filesystem::path& destination,
    std::string_view contents) {
    create_private_file(destination, contents);
}

} // namespace cha::app::vault

namespace cha::app {

std::optional<CommandResult> Application::Impl::VaultMaintenance::submit_welcome_command(
    std::string_view input, std::uint64_t epoch) {
    input = trim_view(input);
    const auto command = vault::welcome_command_name(input);
    if (command.empty()) return std::nullopt;
    const bool download = command == "/download";

    const auto reject = [](std::string message) {
        return CommandResult{.session = {.notice = std::move(message)}};
    };
    if (!app.command.chaweb_host) {
        return reject("Vault commands are available in ChaWeb Welcome.");
    }
    const auto argument = trim_view(input.substr(command.size()));
    const bool confirmed = download && argument == "confirm";
    if (download && !argument.empty() && !confirmed)
        return reject("Type /download or /download confirm.");
    if (!download && argument.empty()) return reject("Type /open <name>.");

    if (app.vault_command_running.exchange(true))
        return reject("A vault command is running. Try again when it finishes.");
    try {
        if (app.background_jobs.launch(
                [this, download, confirmed, target = std::string(argument), epoch]
                (const std::atomic_bool& cancelled) mutable {
                    struct Release {
                        std::atomic_bool& flag;
                        ~Release() { flag = false; }
                    } release{app.vault_command_running};
                    try {
                        if (!cancelled) run_welcome_command(download, confirmed, std::move(target), epoch);
                    } catch (const std::exception& error) {
                        log_warn(std::string("Could not report the vault command result: ") + error.what());
                    }
                })) {
            return CommandResult{.session = {.input_consumed = true}, .clear_input = true};
        }
    } catch (const std::exception& error) {
        log_warn(std::string("Could not launch vault command: ") + error.what());
    }
    app.vault_command_running = false;
    return reject("The vault command could not start.");
}

void Application::Impl::VaultMaintenance::post_welcome_notice(std::string text, std::string error_code) {
    if (app.state.load() != ApplicationState::running || app.stopping_flag) return;
    if (!app.live_sessions->post_session_notice(app.published_epoch.load(),
            {std::string(entrance_id), std::string(welcome_id)}, std::move(text), std::move(error_code))) {
        log_warn("Could not post the vault command notice.");
    }
}

void Application::Impl::VaultMaintenance::run_welcome_command(
    bool download, bool confirmed, std::string target, std::uint64_t epoch) {
    if (download) target = app.current_vault_.get().name;
    bool saved = false;
    const auto report = [&](std::string message, std::string error_code = {}) {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        post_welcome_notice(std::move(message), std::move(error_code));
    };
    const auto failed = [&](std::string reason, std::string error_code = {}) {
        report(saved
            ? "Vault '" + target + "' was saved but could not be opened. " + reason
            : std::string(download ? "Download" : "Open") + " failed for vault '"
                + target + "'. " + reason, std::move(error_code));
    };

    try {
        if (download && confirmed) {
            download_database(epoch, true);
            report("Downloaded and reopened vault '" + target + "'.");
            return;
        }
        PendingContextNotice notice;
        try {
            const std::lock_guard lifecycle(app.lifecycle_mutex);
            app.require_admitted(epoch);
            if (download) {
                const auto active = app.current_vault_.get();
                app.store->require_main();
                if (active.password_protected)
                    throw VaultPasswordError("Welcome cannot download a protected vault.");
                (void)require_r2_key();
                post_welcome_notice("Download will replace vault '" + active.name
                    + "' with the R2 copy. Local changes absent from R2 will survive only "
                      "in the .bac backup. Running work will stop. Type /download confirm to continue.");
                return;
            }
            if (const auto* local = find_vault(app.command.vaults, target)) {
                target = local->name;
                if (same_vault_name(app.current_vault_.get().name, target)) {
                    post_welcome_notice("Vault '" + target + "' is already open.");
                    return;
                }
                switch_vault_locked(*local, {}, notice, true);
            } else {
                app.store->require_main();
                const auto key = require_r2_key();
                target = save_r2_vault_locked(target, key, true).name;
                saved = true;
            }
        } catch (...) {
            notice.dispatch();
            throw;
        }
        notice.dispatch();
        if (saved) switch_vault(target, {}, epoch);
        report(saved ? "Downloaded and opened vault '" + target + "'."
                     : "Opened vault '" + target + "'.");
    } catch (const R2ObjectNotFoundError& error) {
        failed(download ? error.what() : std::string(error.what())
            + " Remote lookup requires the R2 file name without .sqlite3, in its exact case.",
            std::string(to_string(ErrorCode::not_found)));
    } catch (const ApplicationError& error) {
        failed(error.code == ErrorCode::vault_changed
            ? "The vault context changed." : error.what());
    } catch (const std::exception& error) {
        log_warn(std::string("Vault command failed: ") + error.what());
        failed(error.what());
    }
}

R2StorageKey Application::Impl::VaultMaintenance::require_r2_key() const {
    const auto key = app.api_keys->r2();
    if (!key) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The active vault has no R2 key");
    }
    return *key;
}

void Application::Impl::VaultMaintenance::clear_downloaded_inbox(
    const std::filesystem::path& database, std::string_view name) {
    if (app.command.machine.empty()) return;
    const auto credentials = [&] {
        storage::SqliteDatabase snapshot(database, storage::SqliteDatabase::Mode::read_only);
        return sync_read_d1(snapshot);
    }();
    if (!credentials) return;
    D1Client client({credentials->account_id, credentials->database_id, credentials->api_token},
        app.d1_test_transport);
    const auto error = client.clear_queue(sync_queue_table(name, app.command.machine));
    if (error && error->failure != D1Client::Failure::missing_table) {
        throw std::runtime_error("Could not clear the D1 inbox. " + error->message);
    }
}

void Application::Impl::VaultMaintenance::publish_vault_names() {
    std::vector<std::string> names;
    names.reserve(app.command.vaults.size());
    for (const VaultDefinition& vault : app.command.vaults) {
        names.push_back(vault.name);
    }
    app.current_vault_.set_names(std::move(names));
}

void Application::Impl::VaultMaintenance::publish_vault(VaultDefinition vault) {
    std::vector<std::string> names;
    names.reserve(app.command.vaults.size());
    for (const VaultDefinition& configured : app.command.vaults) {
        names.push_back(configured.name);
    }
    app.current_vault_.set(std::move(vault), std::move(names));
}

std::chrono::milliseconds Application::Impl::VaultMaintenance::maintenance_grace() const {
    return app.command.test_shutdown_grace_ms
        ? std::chrono::milliseconds(*app.command.test_shutdown_grace_ms)
        : app.settings.shutdown_grace;
}

std::uint64_t Application::Impl::VaultMaintenance::publish_epoch(
    PendingContextNotice& notice,
    ApplicationState next_state,
    std::optional<SyncStart> sync_start) {
    app.notes->interrupt_note_audio();
    const auto epoch = app.live_sessions->bump_context_epoch();
    // Retire old requests before letting the runtime open sessions again.
    global_maintenance.reset();
    if (sync_start) app.start_sync_worker(*sync_start);
    app.published_epoch.store(epoch);
    ApplicationState expected = ApplicationState::maintenance;
    (void)app.state.compare_exchange_strong(expected, next_state);
    app.take_context_notice(notice);
    return epoch;
}

bool Application::Impl::VaultMaintenance::drain_for_maintenance(
    PendingContextNotice& notice,
    bool cancel_audio, bool require_main) {
    if (require_main) app.store->require_main();
    if (app.unusable) {
        throw WorkspaceRestartRequiredError(
            "The workspace database could not be reopened after an earlier "
            "maintenance operation. Restart is required");
    }
    if (app.stopping_flag || app.stopped
        || app.state.load() != ApplicationState::running) {
        throw std::runtime_error("CHA application is unavailable");
    }
    if (app.vault_upload_running) {
        throw ApplicationError(ErrorCode::invalid_argument,
            "A vault upload is running. Try again when it finishes.");
    }
    app.notes->cancel_compressions_locked();
    app.notes->cancel_summaries_locked();
    app.notes->interrupt_folder_stores_locked();
    auto expected = ApplicationState::running;
    if (!app.state.compare_exchange_strong(expected, ApplicationState::maintenance))
        throw std::runtime_error("CHA application is unavailable");
    // Stop receiving before draining the local writers.
    if (app.sync_worker) app.sync_worker->stop();
    GlobalMaintenanceResult reserved = [&] {
        try {
            return app.live_sessions->reserve_global_maintenance(
                maintenance_grace());
        } catch (...) {
            publish_epoch(notice, ApplicationState::running);
            app.resume_sync_worker();
            throw;
        }
    }();
    if (std::holds_alternative<MaintenanceFailure>(reserved)) {
        // Reservation may already have stopped one or more actors. Keep
        // old queued work from treating the recovered context as intact.
        publish_epoch(notice, ApplicationState::running);
        // Resume after returning: an actor can still own the store lock.
        app.notes->resume_note_audio_locked();
        // Welcome's maintenance requests carry the epoch from its opening. If
        // Welcome is still open, reopen it so that its requests are admitted.
        app.live_sessions->reload_session(
            {std::string(entrance_id), std::string(welcome_id)},
            std::chrono::steady_clock::now() + maintenance_grace());
        app.resume_sync_worker();
        return false;
    }
    global_maintenance = std::move(
        std::get<LiveSessionGlobalMaintenance>(reserved));
    try {
        app.pause_resources(cancel_audio);
    } catch (...) {
        cancel_maintenance_locked(notice);
        throw;
    }
    const auto deadline = std::chrono::steady_clock::now() + maintenance_grace();
    if (!app.providers.cancel_active_until(deadline)) {
        cancel_maintenance_locked(notice);
        return false;
    }
    app.sessions->release_pending();
    // Controllers have finished; outgoing hooks had database access.
    // Collect flags and attempt the final send before the database closes.
    // Failures retain unsent work; maintenance still proceeds.
    try {
        if (app.sync_worker) app.sync_worker->flush_send();
    } catch (const std::exception& error) {
        log_warn(std::string("D1 sync maintenance flush failed: ") + error.what());
    }
    return true;
}

void Application::Impl::VaultMaintenance::cancel_maintenance_locked(PendingContextNotice& notice) {
    app.publish_capabilities_locked();
    publish_epoch(notice, ApplicationState::running);
    app.resume_sync_worker();
    if (app.state.load() != ApplicationState::running
        || app.stopping_flag.load()) {
        return;
    }
    app.resume_resources();
    if (app.stopping_flag.load()) {
        app.state.store(ApplicationState::stopping);
        app.pause_resources(true);
        app.take_context_notice(notice);
    }
}

void Application::Impl::VaultMaintenance::end_maintenance_locked(
    bool available, PendingContextNotice& notice, SyncStart sync_start) {
    if (!available) {
        app.sync_worker.reset();
        app.unusable = true;
        publish_epoch(notice, ApplicationState::unavailable);
        return;
    }
    app.publish_capabilities_locked();
    publish_epoch(notice, ApplicationState::running, sync_start);
    if (app.state.load() != ApplicationState::running
        || app.stopping_flag.load()) {
        return;
    }
    app.resume_resources();
    if (app.stopping_flag.load()) {
        app.state.store(ApplicationState::stopping);
        app.pause_resources(true);
        app.take_context_notice(notice);
    }
}

void Application::Impl::VaultMaintenance::reopen(
    WorkspaceConfigStore::MaintenanceGuard& database,
    const SessionRepository::MaintenanceGuard& repository) {
    try {
        database.reopen();
        app.notes->clear_audio_warnings_locked();
        repository.synchronize_forums(*app.store->snapshot());
        repository.recover_sessions();
    } catch (...) {
        app.unusable = true;
        throw;
    }
}

void Application::Impl::VaultMaintenance::reopen_after_failure(
    WorkspaceConfigStore::MaintenanceGuard& database,
    const SessionRepository::MaintenanceGuard& repository) {
    try {
        reopen(database, repository);
    } catch (const std::exception& error) {
        log_critical(
            "Failed to reopen workspace database after maintenance: "
            + std::string(error.what()));
        throw WorkspaceRestartRequiredError(error.what());
    }
}

void Application::Impl::VaultMaintenance::rebuild_mirror(const std::optional<std::filesystem::path>& root) {
    try {
        const bool main = app.store->is_main();
        if (root && !main) log_warn("Secondary process does not write the Markdown mirror");
        app.mirror->rebuild(main ? root : std::nullopt, *app.sessions);
    } catch (const std::exception& error) {
        log_warn(
            "Session mirror rebuild failed: " + std::string(error.what()));
        app.mirror->rebuild(std::nullopt, *app.sessions);
    }
}

void Application::Impl::VaultMaintenance::delete_forum(
    std::string_view forum_id, std::uint64_t epoch) {
    PendingContextNotice notice;
    try {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
        if (!drain_for_maintenance(notice)) {
            throw std::runtime_error(
                "Could not pause active sessions for database maintenance");
        }
        try {
            workspace::delete_forum(*app.store, *app.live_sessions, forum_id);
            end_maintenance_locked(true, notice);
        } catch (const WorkspaceRestartRequiredError&) {
            end_maintenance_locked(false, notice);
            throw;
        } catch (...) {
            cancel_maintenance_locked(notice);
            throw;
        }
    } catch (...) { notice.dispatch(); throw; }
    notice.dispatch();
}

template<typename Operation>
auto Application::Impl::VaultMaintenance::maintain_database(
    std::uint64_t epoch,
    Operation operation,
    bool cancel_audio, SyncStart sync_start,
    const std::function<void()>& before_drain,
    const std::function<void()>& after_reopen) {
    PendingContextNotice notice;
    using Result = std::invoke_result_t<Operation,
        WorkspaceConfigStore::MaintenanceGuard&, SessionRepository::MaintenanceGuard&>;
    std::optional<Result> result;
    std::exception_ptr error;
    try {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
        app.store->require_main();
        if (before_drain) before_drain();
        if (!drain_for_maintenance(notice, cancel_audio, false)) {
            throw std::runtime_error(
                "Could not pause active sessions for database maintenance");
        }
        try {
            {
                auto database = app.store->reserve_maintenance();
                auto repository = app.sessions->reserve_maintenance();
                database.close();
                try {
                    repository.checkpoint();
                    result.emplace(operation(database, repository));
                } catch (...) {
                    error = std::current_exception();
                }
                // A failed reopen needs a restart, also after a successful operation.
                reopen_after_failure(database, repository);
            }
            end_maintenance_locked(true, notice, error ? SyncStart::resume : sync_start);
            if (!error && after_reopen) after_reopen();
        } catch (const WorkspaceRestartRequiredError&) {
            end_maintenance_locked(false, notice);
            throw;
        } catch (...) {
            if (app.state.load() == ApplicationState::maintenance) {
                cancel_maintenance_locked(notice);
            }
            throw;
        }
    } catch (...) {
        notice.dispatch();
        throw;
    }
    notice.dispatch();
    if (error) std::rethrow_exception(error);
    return std::move(*result);
}

VaultRegistrySnapshot Application::Impl::VaultMaintenance::vault_snapshot() const {
    const std::lock_guard lifecycle(app.lifecycle_mutex);
    return {app.command.vaults, app.current_vault_.get()};
}

VaultDefinition Application::Impl::VaultMaintenance::create_vault(VaultCreate create, std::uint64_t epoch) {
    const std::lock_guard lifecycle(app.lifecycle_mutex);
    app.require_admitted(epoch);
    app.store->require_main();
    const VaultDefinition* copied = nullptr;
    if (create.copy_from) {
        copied = find_vault(app.command.vaults, *create.copy_from);
        if (copied == nullptr) {
            throw std::invalid_argument("The source vault was not found");
        }
    }

    VaultDefinition candidate{
        .name = std::move(create.display_name),
        .password_protected = !create.password.empty(),
        .source = vault::next_vault_file(app.command.config_directory),
        .parent = copied ? std::optional<std::string>(copied->name) : std::nullopt,
    };
    try {
        validate_public_name(candidate.name, "vault_name", candidate.source);
        require_path_component(candidate.name, candidate.source);
    } catch (const std::runtime_error& error) {
        throw std::invalid_argument(error.what());
    }
    candidate.data = vault::normalized_vault_path(
        app.command.config_directory,
        path_from_utf8(candidate.name + ".sqlite3"));
    vault::assign_vault_paths(candidate, app.command);
    vault::require_available_database_path(candidate.data);
    vault::validate_vault_paths(candidate);

    std::vector<VaultDefinition> updated = app.command.vaults;
    updated.push_back(candidate);
    vault::validate_candidate_vaults(app.command.config_directory, updated);

    std::optional<SessionLease> source_use;
    if (copied && copied->data != app.current_vault_.get().data) {
        source_use.emplace(SessionLease::acquire(
            copied->data, vault_in_use_message, SessionLease::Mode::shared));
    }
    auto target_main = SessionLease::acquire_main(candidate.data);
    auto target_use = SessionLease::acquire(candidate.data, vault_in_use_message);
    vault::require_available_database_path(candidate.data);
    write_toml_file(candidate.source, vault::vault_definition_table(candidate));
    const auto remove_unpublished = [&] {
        std::error_code ignored;
        std::filesystem::remove(candidate.source, ignored);
        std::filesystem::remove(candidate.data, ignored);
        for (const auto suffix : {"-journal", "-wal", "-shm"}) {
            auto sidecar = candidate.data;
            sidecar += suffix;
            std::filesystem::remove(sidecar, ignored);
        }
    };
    try {
        if (copied != nullptr) {
            std::string source_password;
            if (copied->password_protected) {
                if (!same_vault_name(
                        copied->name, app.current_vault_.get().name)) {
                    throw std::invalid_argument(
                        "Switch to a protected source vault before copying it");
                }
                source_password = app.active_password;
            }
            copy_workspace_session_database(
                copied->data,
                candidate.data,
                source_password,
                create.password);
            // Clean the destination queue/flags; retain source pending work.
            // The generic copy helper is unchanged for maintenance/password use.
            try {
                storage::SqliteDatabase fresh(
                    candidate.data, storage::SqliteDatabase::Mode::read_write,
                    create.password);
                storage::SqliteTransaction transaction(fresh);
                sync_clear_outgoing(fresh);
                for (const auto& [name, content] : default_prompt_config_files()) {
                    fresh.prepare("INSERT INTO config (name, content) VALUES (?1, ?2)"
                                  " ON CONFLICT(name) DO NOTHING",
                                  std::string_view(name), std::string_view(content)).run();
                }
                transaction.commit();
            } catch (const std::exception& error) {
                log_warn(
                    std::string("New vault initialization failed: ") + error.what());
                throw;
            }
        } else {
            create_workspace_session_database_from_configuration(
                app.current_vault_.get().data,
                candidate.data,
                app.active_password,
                create.password,
                default_prompt_config_files());
        }
    } catch (...) {
        remove_unpublished();
        throw;
    }

    std::sort(
        updated.begin(), updated.end(),
        [](const VaultDefinition& left, const VaultDefinition& right) {
            return fold_ascii(left.name) < fold_ascii(right.name);
        });
    app.command.vaults = std::move(updated);
    publish_vault_names();
    return candidate;
}

VaultDefinition Application::Impl::VaultMaintenance::update_vault(
    std::string_view current_name, VaultUpdate update, std::uint64_t epoch) {
    bool active;
    {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
        app.store->require_main();
        active = same_vault_name(app.current_vault_.get().name, current_name);
    }
    const auto update_target = [&](WorkspaceConfigStore::MaintenanceGuard* database,
                                   SessionRepository::MaintenanceGuard* repository) {
        const auto found = std::find_if(app.command.vaults.begin(), app.command.vaults.end(),
            [&](const VaultDefinition& vault) { return same_vault_name(vault.name, current_name); });
        if (found == app.command.vaults.end()) throw std::out_of_range("The vault was not found");
        const VaultDefinition previous = *found;
        VaultDefinition candidate = previous;
        candidate.name = update.display_name;
        const bool protect = !update.password.empty();
        if (protect && previous.password_protected) {
            throw std::invalid_argument("This vault is already protected");
        }
        if (protect) candidate.password_protected = true;
        vault::assign_vault_paths(candidate, app.command);
        vault::validate_vault_paths(candidate);
        auto updated = app.command.vaults;
        updated[static_cast<std::size_t>(found - app.command.vaults.begin())] = candidate;
        vault::validate_candidate_vaults(app.command.config_directory, updated);

        std::optional<SessionLease> target_main;
        std::optional<SessionLease> target_use;
        if (!active) {
            target_main.emplace(SessionLease::acquire_main(previous.data));
            target_use.emplace(SessionLease::acquire(previous.data, vault_in_use_message));
        }
        std::vector<vault::VaultDirectoryMove> moved;
        bool selection_written = false;
        try {
            vault::move_vault_directory(previous.modify, candidate.modify, "modify", moved);
            vault::move_vault_directory(previous.mirror, candidate.mirror, "mirror", moved);
            write_toml_file(candidate.source, vault::vault_definition_table(candidate));
            if (active) {
                vault::write_vault_selection(app.command.config_directory, candidate.name);
                selection_written = true;
            }
            if (protect) {
                checkpoint_workspace_session_database(previous.data, active ? app.active_password : "");
                protect_workspace_session_database(previous.data, update.password);
                if (active) {
                    app.active_password = update.password;
                    database->set_password(update.password);
                    repository->retarget(previous.data, update.password);
                }
            }
        } catch (...) {
            const bool protection_committed = protect
                && inspect_workspace_session_database(previous.data, update.password)
                    == WorkspaceDatabaseState::valid_v2;
            if (!protection_committed) {
                try {
                    write_toml_file(
                        previous.source,
                        vault::vault_definition_table(previous));
                } catch (const std::exception& error) {
                    log_critical(
                        "Failed to restore vault definition after update: "
                        + std::string(error.what()));
                }
                if (selection_written) {
                    try {
                        vault::write_vault_selection(
                            app.command.config_directory, previous.name);
                    } catch (const std::exception& error) {
                        log_critical(
                            "Failed to restore vault selection after update: "
                            + std::string(error.what()));
                    }
                }
                vault::restore_vault_directories(moved);
            }
            throw;
        }
        std::sort(updated.begin(), updated.end(), [](const auto& left, const auto& right) {
            return fold_ascii(left.name) < fold_ascii(right.name);
        });
        app.command.vaults = std::move(updated);
        if (active) {
            app.command.vault = candidate;
            publish_vault(candidate);
        } else publish_vault_names();
        return candidate;
    };
    if (active) {
        auto candidate = maintain_database(epoch, [&](auto& database, auto& repository) {
            return update_target(&database, &repository);
        });
        rebuild_mirror(session_mirror_root(candidate));
        return candidate;
    }
    const std::lock_guard lifecycle(app.lifecycle_mutex);
    app.require_admitted(epoch);
    app.store->require_main();
    return update_target(nullptr, nullptr);
}

void Application::Impl::VaultMaintenance::delete_vault(std::string_view name, std::uint64_t epoch) {
    const std::lock_guard lifecycle(app.lifecycle_mutex);
    app.require_admitted(epoch);
    app.store->require_main();
    const auto found = std::find_if(
        app.command.vaults.begin(), app.command.vaults.end(),
        [name](const VaultDefinition& vault) {
            return same_vault_name(vault.name, name);
        });
    if (found == app.command.vaults.end()) {
        throw std::out_of_range("The vault was not found");
    }
    if (app.command.vaults.size() == 1) {
        throw std::invalid_argument("The last vault cannot be deleted");
    }
    if (same_vault_name(app.current_vault_.get().name, found->name)) {
        throw std::invalid_argument("The active vault cannot be deleted");
    }
    auto target_main = SessionLease::acquire_main(found->data);
    auto target_use = SessionLease::acquire(found->data, vault_in_use_message);
    (void)std::filesystem::remove(found->source);
    app.command.vaults.erase(found);
    publish_vault_names();
}

std::vector<std::string> Application::Impl::VaultMaintenance::list_r2_vaults(std::uint64_t epoch) const {
    R2StorageKey key;
    std::vector<std::string> local_database_names;
    {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
        const std::optional<R2StorageKey> configured = app.api_keys->r2();
        if (!configured) {
            throw std::invalid_argument(
                "R2 storage credentials are not configured for the active vault");
        }
        key = *configured;
        local_database_names.reserve(app.command.vaults.size());
        for (const VaultDefinition& vault : app.command.vaults) {
            local_database_names.push_back(utf8_path(vault.data.filename()));
        }
    }

    std::vector<std::string> names = list_r2_database_names(
        key, [this] { return app.stopping_flag.load(); });
    std::erase_if(names, [&](const std::string& name) {
        const std::string database_name = name + ".sqlite3";
        return std::ranges::any_of(
            local_database_names,
            [&](const std::string& local) {
                return fold_ascii(local) == fold_ascii(database_name);
            });
    });
    {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
    }
    return names;
}

VaultDefinition Application::Impl::VaultMaintenance::download_r2_vault(
    std::string_view name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(app.lifecycle_mutex);
    app.require_admitted(epoch);
    app.store->require_main();
    const std::optional<R2StorageKey> r2 = app.api_keys->r2();
    if (!r2) {
        throw std::invalid_argument(
            "R2 storage credentials are not configured for the active vault");
    }
    return save_r2_vault_locked(name, *r2, false);
}

VaultDefinition Application::Impl::VaultMaintenance::save_r2_vault_locked(
    std::string_view name, const R2StorageKey& r2, bool from_welcome) {
    if (from_welcome) {
        validate_public_name(name, "vault name", app.command.config_directory);
        require_path_component(name, app.command.config_directory);
    }
    const std::string database_name = std::string(name) + ".sqlite3";
    require_path_component(database_name, app.command.config_directory);

    const auto database = app.command.config_directory / path_from_utf8(database_name);
    if (from_welcome) vault::require_available_database_path(database);
    const VaultDefinition destination{
        .data = vault::normalized_vault_path(app.command.config_directory, database),
        .source = vault::next_vault_file(app.command.config_directory),
    };
    if (!from_welcome) vault::require_available_database_path(destination.data);

    std::vector<VaultDefinition> updated = app.command.vaults;
    VaultDefinition candidate;
    const auto register_candidate = [&](VaultDefinition definition) {
        vault::assign_vault_paths(definition, app.command);
        updated.push_back(definition);
        vault::validate_candidate_vaults(app.command.config_directory, updated);
        std::sort(
            updated.begin(), updated.end(),
            [](const VaultDefinition& left, const VaultDefinition& right) {
                return fold_ascii(left.name) < fold_ascii(right.name);
            });
        candidate = std::move(definition);
    };
    BeforeR2Install before_install;
    if (from_welcome) {
        vault::require_available_database_path(destination.source);
        post_welcome_notice("Downloading vault '" + std::string(name) + "' from R2.");
        before_install = [&](const std::filesystem::path& temporary, VaultDefinition downloaded) {
            register_candidate(std::move(downloaded));
            clear_downloaded_inbox(temporary, candidate.name);
        };
    }

    (void)download_new_database_from_r2(
        destination.data, destination.source, database_name, r2,
        [this] { return app.stopping_flag.load(); }, before_install);
    if (!from_welcome) {
        try {
            register_candidate(load_vault_definition_file(
                app.command.config_directory, destination.source));
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(destination.source, ignored);
            std::filesystem::remove(destination.data, ignored);
            throw;
        }
    }
    app.command.vaults = std::move(updated);
    publish_vault_names();
    return candidate;
}

MaintenanceResult Application::Impl::VaultMaintenance::switch_vault(
    std::string_view name,
    std::string password,
    std::uint64_t epoch) {
    Impl::PendingContextNotice notice;
    MaintenanceResult result;
    try {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
        const VaultDefinition* const target =
            find_vault(app.command.vaults, name);
        if (target == nullptr) {
            throw UnknownVaultError(
                "Unknown vault '" + std::string(name) + "'");
        }
        if (!same_vault_name(app.current_vault_.get().name, target->name)) {
            switch_vault_locked(*target, std::move(password), notice);
        } else {
            app.take_context_notice(notice);
        }
        result = {app.state.load(), app.published_epoch.load()};
    } catch (...) {
        notice.dispatch();
        throw;
    }
    notice.dispatch();
    return result;
}

void Application::Impl::VaultMaintenance::switch_vault_locked(
    const VaultDefinition& selected, std::string password,
    PendingContextNotice& notice, bool from_welcome) {
    if (selected.password_protected && password.empty()) {
        throw VaultPasswordError(
            "Password required to open this vault");
    }

    SessionLease target_lease = SessionLease::acquire(
        selected.data,
        vault_in_use_message,
        SessionLease::Mode::shared);
    if (selected.password_protected) {
        require_openable_protected_database(selected.data, password);
    }
    require_switchable_database(selected.data, password);

    if (from_welcome) post_welcome_notice("Opening vault '" + selected.name + "'.");
    if (!drain_for_maintenance(notice, true, false)) {
        throw std::runtime_error(
            "Could not pause active sessions for database maintenance");
    }
    bool database_closed = false;
    try {
        {
            auto database = app.store->reserve_maintenance();
            SessionRepository::MaintenanceGuard repository =
                app.sessions->reserve_maintenance();
            database.close(false);
            database_closed = true;
            database.retarget(
                selected.data, std::move(target_lease), password);
            repository.retarget(selected.data, password);
            app.active_password = std::move(password);
            reopen(database, repository);
        }
        app.current_vault_.set(selected);
        app.command.vault = selected;
        try {
            // Migration edits the store and takes its lock again.
            // Keep application maintenance active, but release the
            // database guards before starting that edit.
            app.api_keys->migrate_vault();
        } catch (const std::exception& error) {
            log_warn(
                "Legacy API-key migration failed after switching vault: "
                + std::string(error.what()));
        }
        end_maintenance_locked(true, notice,
            app.command.chaweb_host ? SyncStart::recover : SyncStart::open);
    } catch (...) {
        if (app.unusable) {
            end_maintenance_locked(false, notice);
        } else if (
            app.state.load() == ApplicationState::maintenance) {
            if (database_closed) {
                end_maintenance_locked(false, notice);
            } else {
                cancel_maintenance_locked(notice);
            }
        }
        throw;
    }
    rebuild_mirror(session_mirror_root(selected));
    try {
        if (app.store->is_main()) vault::write_vault_selection(
            app.command.config_directory, selected.name);
    } catch (const std::exception& error) {
        log_warn(
            "Failed to persist vault selection: "
            + std::string(error.what()));
    }
}

MaintenanceResult Application::Impl::VaultMaintenance::merge_vault(
    std::string_view source_name,
    std::string password,
    std::uint64_t epoch) {
    Impl::PendingContextNotice notice;
    MaintenanceResult result;
    try {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
        app.store->require_main();
        const auto current = app.current_vault_.get();
        const VaultDefinition* const source =
            find_vault(app.command.vaults, source_name);
        if (source == nullptr) {
            throw UnknownVaultError(
                "Unknown vault '" + std::string(source_name) + "'");
        }
        if (same_vault_name(current.name, source->name)) {
            throw std::invalid_argument("Cannot merge a vault into itself");
        }
        if (source->password_protected && password.empty()) {
            throw VaultPasswordError("Password required to open this vault");
        }

        VaultDefinition selected = *source;
        auto source_main = SessionLease::acquire_main(selected.data);
        SessionLease source_lease = SessionLease::acquire(
            selected.data,
            vault_in_use_message);
        if (selected.password_protected) {
            require_openable_protected_database(selected.data, password);
        }
        if (!drain_for_maintenance(notice)) {
            throw std::runtime_error(
                "Could not pause active sessions for database maintenance");
        }
        bool merged = false;
        try {
            app.store->merge(selected.data, source_lease, password);
            app.notes->clear_audio_warnings_locked();
            merged = true;
            {
                SessionRepository::MaintenanceGuard repository =
                    app.sessions->reserve_maintenance();
                repository.synchronize_forums(*app.store->snapshot());
            }
            rebuild_mirror(
                session_mirror_root(app.current_vault_.get()));
        } catch (const WorkspaceRestartRequiredError&) {
            end_maintenance_locked(false, notice);
            throw;
        } catch (const std::exception& error) {
            if (app.state.load() == ApplicationState::maintenance) {
                if (!merged) {
                    cancel_maintenance_locked(notice);
                    throw;
                }
                end_maintenance_locked(false, notice);
            }
            throw WorkspaceRestartRequiredError(
                std::string(
                    "Configuration was committed but forums could not be "
                    "synchronized: ")
                + error.what() + ". Restart is required");
        } catch (...) {
            if (app.state.load() == ApplicationState::maintenance) {
                if (!merged) {
                    cancel_maintenance_locked(notice);
                    throw;
                }
                end_maintenance_locked(false, notice);
            }
            throw WorkspaceRestartRequiredError(
                "Configuration was committed but forums could not be "
                "synchronized. Restart is required");
        }
        end_maintenance_locked(true, notice);
        result = {app.state.load(), app.published_epoch.load()};
    } catch (...) {
        notice.dispatch();
        throw;
    }
    notice.dispatch();
    return result;
}

// The copy is made in maintenance. From then until finish_upload ends, other
// maintenance is refused, so the saved ETag still describes the active vault.
R2DatabaseUpload Application::Impl::VaultMaintenance::prepare_upload(
    std::uint64_t epoch, VaultDefinition& vault, R2StorageKey& storage) {
    bool reserved = false;
    try {
        return maintain_database(epoch, [&](auto&, auto&) {
            const std::optional<R2StorageKey> r2 = app.api_keys->r2();
            if (!r2) throw ApplicationError(
                ErrorCode::invalid_argument, "The active vault has no R2 key");
            vault = app.current_vault_.get();
            storage = *r2;
            // Writers are excluded; the maintenance drain already collected
            // flags and attempted one send. Refuse while pending work remains.
            {
                storage::SqliteDatabase pending_db(
                    vault.data, storage::SqliteDatabase::Mode::read_only,
                    app.active_password);
                if (sync_has_pending(pending_db)) {
                    throw ApplicationError(
                        ErrorCode::invalid_argument,
                        "Changes are not sent to D1 yet. Try again when D1 can be reached.");
                }
            }
            R2DatabaseUpload upload(vault.data, vault.source, app.active_password);
            app.vault_upload_running = true;
            reserved = true;
            return upload;
        }, false);
    } catch (...) {
        if (reserved) app.vault_upload_running = false;
        throw;
    }
}

R2DatabaseTransfer Application::Impl::VaultMaintenance::finish_upload(
    R2DatabaseUpload upload, const VaultDefinition& vault, const R2StorageKey& storage,
    std::string_view expected_etag, const std::function<bool()>& cancelled) {
    struct EndUpload {
        std::atomic_bool& running;
        ~EndUpload() { running = false; }
    } end{app.vault_upload_running};
    return upload.upload(storage, cancelled, expected_etag, [this, &vault](std::string_view etag) {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        const auto configured = std::find_if(
            app.command.vaults.begin(), app.command.vaults.end(),
            [&](const VaultDefinition& candidate) {
                return candidate.source == vault.source;
            });
        if (configured == app.command.vaults.end()
            || app.current_vault_.get().source != vault.source
            || !app.store->is_main()) return;
        save_r2_vault_etag(vault.source, etag);
        configured->r2_etag = std::string(etag);
        app.command.vault = *configured;
        publish_vault(*configured);
    });
}

// The bridge worker makes the copy. The transfer can take minutes, so it runs
// as a background job and the worker can serve other requests meanwhile.
std::shared_ptr<OperationReply>
Application::Impl::VaultMaintenance::start_database_upload(
    std::optional<std::string> expected_etag,
    std::uint64_t epoch) {
    const std::string etag = expected_etag.value_or("");
    VaultDefinition vault;
    R2StorageKey storage;
    auto upload = std::make_shared<R2DatabaseUpload>(prepare_upload(epoch, vault, storage));
    auto reply = std::make_shared<OperationReply>();
    const auto transfer = [this, reply, upload, vault, storage, etag](std::atomic_bool& cancel) {
        try {
            const auto transferred = finish_upload(std::move(*upload), vault, storage, etag,
                [this, &cancel] { return cancel.load() || app.stopping_flag.load(); });
            reply->complete({{"byte_count", transferred.byte_count},
                {"context_epoch", app.published_epoch.load()}});
        } catch (const R2HttpStatusError& error) {
            reply->fail(ErrorCode::invalid_argument, error.what());
        } catch (const std::exception&) {
            // R2DatabaseUpload logs the reason; the reply stays generic.
            reply->fail(app.state.load() == ApplicationState::running
                    ? ErrorCode::internal_error : ErrorCode::application_unavailable,
                "The request could not be completed.");
        }
    };
    bool launched = false;
    try {
        launched = app.background_jobs.launch(transfer);
    } catch (...) {
        app.vault_upload_running = false;
        throw;
    }
    if (!launched) {
        app.vault_upload_running = false;
        reply->fail(ErrorCode::application_unavailable, "The request could not be completed.");
    }
    return reply;
}

R2UploadCheck
Application::Impl::VaultMaintenance::check_database_upload(
    std::uint64_t epoch) const {
    R2StorageKey storage;
    VaultDefinition vault;
    {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
        const std::optional<R2StorageKey> configured = app.api_keys->r2();
        if (!configured) {
            throw ApplicationError(
                ErrorCode::invalid_argument, "The active vault has no R2 key");
        }
        storage = *configured;
        vault = app.current_vault_.get();
    }
    if (!vault.r2_etag) return {};
    const std::optional<std::string> etag = get_r2_database_etag(
        utf8_path(vault.data.filename()), storage,
        [this] { return app.stopping_flag.load(); });
    {
        const std::lock_guard lifecycle(app.lifecycle_mutex);
        app.require_admitted(epoch);
    }
    return {
        .etag = etag,
        .status = vault.r2_etag == etag ? R2EtagStatus::match
            : R2EtagStatus::mismatch,
    };
}

R2DatabaseTransfer Application::Impl::VaultMaintenance::download_database(
    std::uint64_t epoch, bool from_welcome) {
    std::optional<VaultDefinition> downloaded_vault;
    std::optional<R2StorageKey> checked_key;
    return maintain_database(
        epoch,
        [&](auto&, auto&) {
            const auto r2 = checked_key ? *checked_key : require_r2_key();
            const VaultDefinition active = app.current_vault_.get();
            BeforeR2Install before_install;
            if (from_welcome) {
                before_install = [&](const std::filesystem::path& temporary, VaultDefinition definition) {
                    if (definition.password_protected) {
                        throw VaultPasswordError("Welcome cannot download a protected vault.");
                    }
                    clear_downloaded_inbox(temporary, definition.name);
                };
            }
            const auto transferred = download_database_from_r2(
                active.data, active.source, r2, R2DatabaseLease::already_held,
                app.active_password, [this] { return app.stopping_flag.load(); }, before_install);
            downloaded_vault = load_vault_definition_file(app.command.config_directory, active.source);
            vault::assign_vault_paths(*downloaded_vault, app.command);
            const auto configured = std::find_if(
                app.command.vaults.begin(), app.command.vaults.end(),
                [&](const VaultDefinition& candidate) {
                    return candidate.source == active.source;
                });
            if (configured == app.command.vaults.end()) {
                throw std::runtime_error(
                    "The downloaded active vault is not in the vault registry");
            }
            *configured = *downloaded_vault;
            app.command.vault = *downloaded_vault;
            publish_vault(*downloaded_vault);
            return transferred;
        }, true, SyncStart::recover,
        [&] {
            if (!from_welcome) return;
            const auto active = app.current_vault_.get();
            if (active.password_protected)
                throw VaultPasswordError("Welcome cannot download a protected vault.");
            checked_key = require_r2_key();
            post_welcome_notice("Downloading vault '" + active.name + "' from R2.");
        },
        [&] { rebuild_mirror(session_mirror_root(*downloaded_vault)); });
}

WorkspaceConfigTransfer Application::Impl::VaultMaintenance::import_configuration(
    std::uint64_t epoch) {
    return maintain_database(epoch, [this](auto&, auto&) {
        const VaultDefinition vault = app.current_vault_.get();
        if (!vault.modify) {
            throw ApplicationError(
                ErrorCode::invalid_argument,
                "Application config requires 'modify' for Import");
        }
        return import_workspace_configuration(
            *vault.modify,
            vault.data,
            WorkspaceConfigLease::already_held,
            app.active_password,
            app.command.machine);
    });
}

WorkspaceConfigTransfer Application::Impl::VaultMaintenance::export_configuration(
    std::uint64_t epoch) {
    return maintain_database(epoch, [this](auto&, auto&) {
        const VaultDefinition vault = app.current_vault_.get();
        if (!vault.modify) {
            throw ApplicationError(
                ErrorCode::invalid_argument,
                "Application config requires 'modify' for Export");
        }
        vault::clear_existing_export(*vault.modify);
        return export_workspace_configuration(
            vault.data,
            *vault.modify,
            WorkspaceConfigLease::already_held,
            app.active_password);
    }, false);
}

} // namespace cha::app
