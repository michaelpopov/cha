#include "application/session_source.h"

#include "application/builtins.h"
#include "application/workspace_inventory.h"
#include "application/welcome_storage.h"
#include "agents/agent.h"
#include "session/catalog_lease.h"
#include "session/session_catalog.h"
#include "session/session_database.h"
#include "util/logging.h"
#include "util/text.h"

#include <stdexcept>

namespace cha {
namespace {

OpenedSession build_entrance_session(
    SharedPersonaRoster personas,
    PreparedSession prepared,
    WakeNotifier& notifier,
    std::vector<AgentDefinition> definitions) {
    const SessionRestore restored = load_session_state(prepared.database_path);
    return {
        .descriptor = {.identity = {std::string(entrance_id), prepared.session.id},
            .forum_display_name = std::string(entrance_name),
            .session_label = prepared.session.label,
            .forum_default_character_id = std::string(assistant_id)},
        .controller = SessionController::from_shared_definitions(
            std::move(definitions), std::move(personas), std::string(assistant_id),
            prepared.database_path, std::move(prepared.lease), notifier, restored)};
}

} // namespace

OpenedSession open_entrance_session(
    const Workspace& workspace,
    SharedPersonaRoster personas,
    std::string_view inventory,
    PreparedSession prepared,
    WakeNotifier& notifier) {
    auto definitions = builtin_assistant_definitions(
        workspace.workspace_config().provider, std::string(inventory), *personas);
    return build_entrance_session(
        std::move(personas), std::move(prepared), notifier, std::move(definitions));
}

namespace {
[[noreturn]] void rethrow_public_session_error(
    const SessionNameAmbiguousError&, std::string_view forum, std::string_view session) {
    throw SessionNameAmbiguousError("Session name '" + std::string(session)
        + "' is ambiguous in forum '" + std::string(forum) + "'");
}

[[noreturn]] void rethrow_public_session_error(
    const SessionNotFoundError&, std::string_view forum, std::string_view session) {
    throw SessionNotFoundError("Session '" + std::string(session)
        + "' does not exist in forum '" + std::string(forum) + "'");
}

[[noreturn]] void rethrow_public_session_error(
    const SessionNameExistsError&, std::string_view forum, std::string_view session) {
    throw SessionNameExistsError("Session '" + std::string(session)
        + "' already exists in forum '" + std::string(forum) + "'; use /open");
}

[[noreturn]] void rethrow_public_session_error(
    const SessionBusyError&, std::string_view, std::string_view session) {
    throw SessionBusyError("Session '" + std::string(session) + "' is in use elsewhere");
}

class StoredEntranceSessionSource final : public SessionSource {
public:
    StoredEntranceSessionSource(const Workspace& workspace, SharedPersonaRoster personas, const WorkspaceInventory& inventory)
        : workspace_(workspace), personas_(std::move(personas)), inventory_(inventory.serialize()),
          directory_(workspace.root() / "var" / "system" / "entrance" / "sessions") {}
    std::vector<SessionSummary> list() const override {
        try {
            std::vector<SessionSummary> output;
            for (const Session& item : SessionCatalog(directory_, std::string(entrance_id)).list_by_name()) {
                output.push_back({.id = item.id, .label = item.label, .error = item.error, .ambiguous = item.ambiguous});
            }
            return output;
        } catch (const std::exception& error) {
            log_warn("Failed to list Entrance sessions: " + std::string(error.what()));
            throw PublicApplicationError("Unable to list sessions in forum 'Entrance'");
        }
    }
    OpenedSession open(std::string_view name, WakeNotifier& notifier) override {
        SessionCatalog catalog(directory_, std::string(entrance_id));
        try {
            const Session stored = catalog.session_by_name(std::string(name));
            const auto path = catalog.open_database_path(stored.id);
            SessionLease lease = SessionLease::acquire(path);
            return open_entrance_session(workspace_, personas_, inventory_,
                {stored, path, std::move(lease)}, notifier);
        } catch (const SessionNameAmbiguousError& error) { rethrow_public_session_error(error, entrance_name, name); }
        catch (const SessionNotFoundError& error) { rethrow_public_session_error(error, entrance_name, name); }
        catch (const SessionBusyError& error) { rethrow_public_session_error(error, entrance_name, name); }
        catch (const InvalidSessionNameError& error) { throw PublicApplicationError(error.what()); }
        catch (const std::exception& error) {
            log_warn("Failed to open Entrance session: " + std::string(error.what()));
            throw PublicApplicationError("Unable to open session '" + std::string(name)
                + "' in forum 'Entrance'");
        }
    }
    OpenedSession create(std::string name, WakeNotifier& notifier) override {
        SessionCatalog catalog(directory_, std::string(entrance_id));
        const std::string public_name = name;
        try {
            auto definitions = builtin_assistant_definitions(
                workspace_.workspace_config().provider, inventory_, *personas_);
            PreparedSession prepared = catalog.create_by_name(std::move(name));
            try {
                return build_entrance_session(personas_, std::move(prepared), notifier,
                    std::move(definitions));
            } catch (const std::exception& error) {
                log_warn("Created Entrance session could not be opened: " + std::string(error.what()));
                throw SessionCreatedOpenError("session initialization failed");
            }
        } catch (const SessionNameExistsError& error) { rethrow_public_session_error(error, entrance_name, public_name); }
        catch (const InvalidSessionNameError& error) { throw PublicApplicationError(error.what()); }
        catch (const CatalogBusyError&) {
            throw PublicApplicationError(
                "Session catalog for forum 'Entrance' is busy; try again");
        }
        catch (const SessionCreatedOpenError&) { throw; }
        catch (const std::exception& error) {
            log_warn("Failed to create Entrance session: " + std::string(error.what()));
            throw PublicApplicationError("Unable to create session '" + public_name + "' in forum 'Entrance'");
        }
    }
private:
    const Workspace& workspace_;
    SharedPersonaRoster personas_;
    std::string inventory_;
    std::filesystem::path directory_;
};

class WorkspaceSessionSource final : public SessionSource {
public:
    WorkspaceSessionSource(const Workspace& workspace, Forum forum, SharedPersonaRoster personas)
        : workspace_(workspace), forum_(std::move(forum)), personas_(std::move(personas)),
          directory_(forum_.directory / "sessions") {}
    std::vector<SessionSummary> list() const override {
        try {
            std::vector<SessionSummary> output;
            for (const Session& item : SessionCatalog(directory_, forum_.name).list_by_name()) {
                output.push_back({.id = item.id, .label = item.label, .error = item.error, .ambiguous = item.ambiguous});
            }
            return output;
        } catch (const std::exception& error) {
            log_warn("Failed to list sessions for forum_id=" + forum_.name
                + ": " + error.what());
            throw PublicApplicationError("Unable to list sessions in forum '"
                + forum_.display_name + "'");
        }
    }
    OpenedSession open(std::string_view name, WakeNotifier& notifier) override {
        SessionCatalog catalog(directory_, forum_.name);
        try {
            const Session stored = catalog.session_by_name(std::string(name));
            const auto path = catalog.open_database_path(stored.id);
            SessionLease lease = SessionLease::acquire(path);
            return build(stored, path, std::move(lease), notifier,
                load_definitions());
        } catch (const SessionNameAmbiguousError& error) { rethrow_public_session_error(error, forum_.display_name, name); }
        catch (const SessionNotFoundError& error) { rethrow_public_session_error(error, forum_.display_name, name); }
        catch (const SessionBusyError& error) { rethrow_public_session_error(error, forum_.display_name, name); }
        catch (const InvalidSessionNameError& error) { throw PublicApplicationError(error.what()); }
        catch (const std::exception& error) {
            log_warn("Failed to open session for forum_id=" + forum_.name
                + ": " + error.what());
            throw PublicApplicationError("Unable to open session '" + std::string(name)
                + "' in forum '" + forum_.display_name + "'");
        }
    }
    OpenedSession create(std::string name, WakeNotifier& notifier) override {
        SessionCatalog catalog(directory_, forum_.name);
        const std::string public_name = name;
        try {
            auto definitions = load_definitions();
            PreparedSession prepared = catalog.create_by_name(std::move(name));
            try {
                return build(prepared.session, prepared.database_path,
                    std::move(prepared.lease), notifier, std::move(definitions));
            } catch (const std::exception& error) {
                log_warn("Created session could not be opened for forum_id="
                    + forum_.name + ": " + error.what());
                throw SessionCreatedOpenError("session initialization failed");
            }
        } catch (const SessionNameExistsError& error) { rethrow_public_session_error(error, forum_.display_name, public_name); }
        catch (const InvalidSessionNameError& error) { throw PublicApplicationError(error.what()); }
        catch (const CatalogBusyError&) {
            throw PublicApplicationError("Session catalog for forum '"
                + forum_.display_name + "' is busy; try again");
        }
        catch (const SessionCreatedOpenError&) { throw; }
        catch (const std::exception& error) {
            log_warn("Failed to create session for forum_id=" + forum_.name
                + ": " + error.what());
            throw PublicApplicationError("Unable to create session '" + public_name + "' in forum '" + forum_.display_name + "'");
        }
    }
private:
    std::vector<AgentDefinition> load_definitions() const {
        std::vector<AgentDefinitionSource> sources;
        for (const std::string& character : forum_.character_names) {
            sources.push_back({.definition_directory = workspace_.root() / "characters" / character,
                               .member_directory = forum_.directory / "members" / character});
        }
        const auto defaults = forum_.directory / "members" / "character_defaults.toml";
        const std::optional<std::filesystem::path> base = std::filesystem::exists(defaults)
            ? std::optional<std::filesystem::path>(defaults) : std::nullopt;
        return load_agent_definitions(sources, forum_.directory, forum_.display_name,
            *personas_, base, workspace_.workspace_config().provider);
    }
    OpenedSession build(const Session& stored, const std::filesystem::path& path,
                        SessionLease lease, WakeNotifier& notifier,
                        std::vector<AgentDefinition> definitions) const {
        return {.descriptor = {.identity = {forum_.name, stored.id}, .forum_display_name = forum_.display_name, .session_label = stored.label, .forum_default_character_id = forum_.default_agent_id},
                .controller = SessionController::from_shared_definitions(std::move(definitions), personas_, forum_.default_agent_id, path, std::move(lease), notifier, load_session_state(path))};
    }
    const Workspace& workspace_;
    Forum forum_;
    SharedPersonaRoster personas_;
    std::filesystem::path directory_;
};

class WelcomeSessionSource final {
public:
    WelcomeSessionSource(const Workspace& workspace, SharedPersonaRoster personas,
                         const WorkspaceInventory& inventory, WelcomeStorage& storage)
        : workspace_(workspace), personas_(std::move(personas)), inventory_(inventory.serialize()), storage_(storage) {}
    OpenedSession open(WakeNotifier& notifier) {
        PreparedSession prepared = storage_.prepare();
        return open_entrance_session(workspace_, personas_, inventory_,
            std::move(prepared), notifier);
    }
private:
    const Workspace& workspace_;
    SharedPersonaRoster personas_;
    std::string inventory_;
    WelcomeStorage& storage_;
};

class EntranceSessionSource final : public SessionSource {
public:
    EntranceSessionSource(
        std::unique_ptr<SessionSource> stored,
        std::unique_ptr<WelcomeSessionSource> welcome)
        : stored_(std::move(stored)), welcome_(std::move(welcome)) {}

    std::vector<SessionSummary> list() const override {
        return stored_->list();
    }

    OpenedSession open(std::string_view name, WakeNotifier& notifier) override {
        if (fold_ascii(name) == fold_ascii(welcome_name)) {
            return welcome_->open(notifier);
        }
        return stored_->open(name, notifier);
    }

    OpenedSession create(std::string name, WakeNotifier& notifier) override {
        if (fold_ascii(name) == fold_ascii(welcome_name)) {
            throw PublicApplicationError(
                "Session 'Welcome' is reserved in forum 'Entrance'");
        }
        return stored_->create(std::move(name), notifier);
    }

private:
    std::unique_ptr<SessionSource> stored_;
    std::unique_ptr<WelcomeSessionSource> welcome_;
};
}
std::unique_ptr<SessionSource> make_entrance_session_source(
    const Workspace& workspace,
    SharedPersonaRoster personas,
    const WorkspaceInventory& inventory,
    WelcomeStorage& welcome_storage) {
    auto stored = std::make_unique<StoredEntranceSessionSource>(
        workspace, personas, inventory);
    auto welcome = std::make_unique<WelcomeSessionSource>(
        workspace, std::move(personas), inventory, welcome_storage);
    return std::make_unique<EntranceSessionSource>(
        std::move(stored), std::move(welcome));
}

std::unique_ptr<SessionSource> make_workspace_session_source(const Workspace& workspace, Forum forum, SharedPersonaRoster personas) {
    return std::make_unique<WorkspaceSessionSource>(workspace, std::move(forum), std::move(personas));
}

} // namespace cha
