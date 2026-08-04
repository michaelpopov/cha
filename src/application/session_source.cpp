#include "application/session_source.h"

#include "application/builtins.h"
#include "application/workspace_inventory.h"
#include "application/welcome_storage.h"
#include "agents/agent.h"
#include "session/session_catalog.h"
#include "session/session_database.h"
#include "util/text.h"

#include <stdexcept>

namespace cha {
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

class EntranceSessionSource final : public SessionSource {
public:
    EntranceSessionSource(const Workspace& workspace, SharedPersonaRoster personas, const WorkspaceInventory& inventory)
        : workspace_(workspace), personas_(std::move(personas)), inventory_(inventory.serialize()),
          directory_(workspace.root() / "var" / "system" / "entrance" / "sessions") {}
    std::vector<SessionSummary> list() const override {
        std::vector<SessionSummary> output;
        for (const Session& item : SessionCatalog(directory_, "builtin-entrance").list_by_name()) {
            output.push_back({.id = item.id, .label = item.label, .error = item.error, .ambiguous = item.ambiguous});
        }
        return output;
    }
    OpenedSession open(std::string_view name, WakeNotifier& notifier) override {
        if (fold_ascii(name) == fold_ascii(welcome_name)) throw std::runtime_error("Welcome is an ephemeral session source");
        SessionCatalog catalog(directory_, "builtin-entrance");
        try {
            const Session stored = catalog.session_by_name(std::string(name));
            const auto path = catalog.open_database_path(stored.id);
            SessionLease lease = SessionLease::acquire(path);
            return build(stored, path, std::move(lease), notifier);
        } catch (const SessionNameAmbiguousError& error) { rethrow_public_session_error(error, entrance_name, name); }
        catch (const SessionNotFoundError& error) { rethrow_public_session_error(error, entrance_name, name); }
        catch (const SessionBusyError& error) { rethrow_public_session_error(error, entrance_name, name); }
        catch (const std::runtime_error&) {
            throw std::runtime_error("Unable to open session '" + std::string(name)
                + "' in forum 'Entrance'");
        }
    }
    OpenedSession create(std::string name, WakeNotifier& notifier) override {
        if (fold_ascii(name) == fold_ascii(welcome_name)) throw std::runtime_error("Session 'Welcome' is reserved in forum 'Entrance'");
        SessionCatalog catalog(directory_, "builtin-entrance");
        const std::string public_name = name;
        try {
            PreparedSession prepared = catalog.create_by_name(std::move(name));
            return build(prepared.session, prepared.database_path, std::move(prepared.lease), notifier);
        } catch (const SessionNameExistsError& error) { rethrow_public_session_error(error, entrance_name, public_name); }
    }
private:
    OpenedSession build(const Session& stored, const std::filesystem::path& path, SessionLease lease, WakeNotifier& notifier) const {
        auto definitions = builtin_assistant_definitions(workspace_.app_config().provider, inventory_, *personas_);
        return {.descriptor = {.identity = {"builtin-entrance", stored.id}, .forum_display_name = std::string(entrance_name), .session_label = stored.label},
                .controller = SessionController::from_shared_definitions(std::move(definitions), personas_, "builtin-assistant", path, std::move(lease), notifier, load_session_state(path))};
    }
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
        std::vector<SessionSummary> output;
        for (const Session& item : SessionCatalog(directory_, forum_.name).list_by_name()) {
            output.push_back({.id = item.id, .label = item.label, .error = item.error, .ambiguous = item.ambiguous});
        }
        return output;
    }
    OpenedSession open(std::string_view name, WakeNotifier& notifier) override {
        SessionCatalog catalog(directory_, forum_.name);
        try {
            const Session stored = catalog.session_by_name(std::string(name));
            const auto path = catalog.open_database_path(stored.id);
            SessionLease lease = SessionLease::acquire(path);
            return build(stored, path, std::move(lease), notifier);
        } catch (const SessionNameAmbiguousError& error) { rethrow_public_session_error(error, forum_.display_name, name); }
        catch (const SessionNotFoundError& error) { rethrow_public_session_error(error, forum_.display_name, name); }
        catch (const SessionBusyError& error) { rethrow_public_session_error(error, forum_.display_name, name); }
        catch (const std::runtime_error&) {
            throw std::runtime_error("Unable to open session '" + std::string(name)
                + "' in forum '" + forum_.display_name + "'");
        }
    }
    OpenedSession create(std::string name, WakeNotifier& notifier) override {
        SessionCatalog catalog(directory_, forum_.name);
        const std::string public_name = name;
        try {
            PreparedSession prepared = catalog.create_by_name(std::move(name));
            return build(prepared.session, prepared.database_path, std::move(prepared.lease), notifier);
        } catch (const SessionNameExistsError& error) { rethrow_public_session_error(error, forum_.display_name, public_name); }
    }
private:
    OpenedSession build(const Session& stored, const std::filesystem::path& path, SessionLease lease, WakeNotifier& notifier) const {
        std::vector<AgentDefinitionSource> sources;
        for (const std::string& character : forum_.character_names) {
            sources.push_back({.definition_directory = workspace_.root() / "characters" / character,
                               .member_directory = forum_.directory / "members" / character});
        }
        const auto defaults = forum_.directory / "members" / "character_defaults.toml";
        const std::optional<std::filesystem::path> base = std::filesystem::exists(defaults)
            ? std::optional<std::filesystem::path>(defaults) : std::nullopt;
        auto definitions = load_agent_definitions(sources, forum_.directory, forum_.display_name, *personas_, base, workspace_.app_config().provider);
        return {.descriptor = {.identity = {forum_.name, stored.id}, .forum_display_name = forum_.display_name, .session_label = stored.label},
                .controller = SessionController::from_shared_definitions(std::move(definitions), personas_, forum_.default_agent_id, path, std::move(lease), notifier, load_session_state(path))};
    }
    const Workspace& workspace_;
    Forum forum_;
    SharedPersonaRoster personas_;
    std::filesystem::path directory_;
};

class WelcomeSessionSource final : public SessionSource {
public:
    WelcomeSessionSource(const Workspace& workspace, SharedPersonaRoster personas,
                         const WorkspaceInventory& inventory, WelcomeStorage& storage)
        : workspace_(workspace), personas_(std::move(personas)), inventory_(inventory.serialize()), storage_(storage) {}
    std::vector<SessionSummary> list() const override { return {}; }
    OpenedSession open(std::string_view name, WakeNotifier& notifier) override {
        if (fold_ascii(name) != fold_ascii(welcome_name)) throw std::runtime_error("Unknown session");
        PreparedSession prepared = storage_.prepare();
        auto definitions = builtin_assistant_definitions(workspace_.app_config().provider, inventory_, *personas_);
        return {.descriptor = {.identity = {"builtin-entrance", "builtin-welcome"}, .forum_display_name = std::string(entrance_name), .session_label = std::string(welcome_name)},
                .controller = SessionController::from_shared_definitions(std::move(definitions), personas_, "builtin-assistant", prepared.database_path, std::move(prepared.lease), notifier, load_session_state(prepared.database_path))};
    }
    OpenedSession create(std::string, WakeNotifier&) override { throw std::runtime_error("Welcome cannot be created"); }
private:
    const Workspace& workspace_;
    SharedPersonaRoster personas_;
    std::string inventory_;
    WelcomeStorage& storage_;
};
}
std::unique_ptr<SessionSource> make_entrance_session_source(const Workspace& workspace, SharedPersonaRoster personas, const WorkspaceInventory& inventory) {
    return std::make_unique<EntranceSessionSource>(workspace, std::move(personas), inventory);
}

std::unique_ptr<SessionSource> make_workspace_session_source(const Workspace& workspace, Forum forum, SharedPersonaRoster personas) {
    return std::make_unique<WorkspaceSessionSource>(workspace, std::move(forum), std::move(personas));
}

std::unique_ptr<SessionSource> make_welcome_session_source(const Workspace& workspace, SharedPersonaRoster personas, const WorkspaceInventory& inventory, WelcomeStorage& storage) {
    return std::make_unique<WelcomeSessionSource>(workspace, std::move(personas), inventory, storage);
}
} // namespace cha
