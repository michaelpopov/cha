#pragma once

#include "workspace/builtins.h"
#include "session/session_open.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"
#include "providers/providers.h"
#include "storage/session_repository.h"
#include "support/test_workspace.h"
#include "runtime/live_session.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace cha::test {

// The production web graph over one fixture workspace: import into a unified
// database, then open the runtime store.
class WebGraph {
public:
    explicit WebGraph(std::filesystem::path root)
        : root_(std::move(root)),
          providers(std::make_shared<Providers>()) {
        store = WorkspaceConfigStore::open(import_test_database(root_));
        repository = std::make_shared<const SessionRepository>(
            [config = store.get()] { return config->snapshot(); },
            store->database_path(),
            store->workspace_path(),
            store->welcome_path(),
            TemporarySessionSeed{
                {std::string(entrance_id), std::string(welcome_id)},
                std::string(welcome_name)});
    }

    const std::filesystem::path& root() const noexcept { return root_; }

    std::shared_ptr<const SessionRepository> sessions() const {
        return repository;
    }

    SessionOpener opener() const {
        return [repository = repository, providers = providers, config = store.get()](
                   const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            return open_session(
                *repository, identity, *providers, std::move(notifier), *config);
        };
    }

    static FullSessionId welcome() {
        return {std::string(entrance_id), std::string(welcome_id)};
    }

private:
    std::filesystem::path root_;

public:
    std::shared_ptr<Providers> providers;
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<const SessionRepository> repository;
};

} // namespace cha::test
