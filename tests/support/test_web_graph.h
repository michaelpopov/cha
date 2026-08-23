#pragma once

#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace.h"
#include "providers/providers.h"
#include "session/session_repository.h"
#include "web/live_session.h"
#include "web/lobby_routes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace cha::test {

// The production web graph over one fixture workspace.
class WebGraph {
public:
    explicit WebGraph(std::filesystem::path root)
        : root_(std::move(root)),
          providers(std::make_shared<Providers>()),
          repository(std::make_shared<const SessionRepository>(
              root_,
              TemporarySessionSeed{
                  {std::string(entrance_id), std::string(welcome_id)},
                  std::string(welcome_name)})) {
        loadws(root_);
    }

    const std::filesystem::path& root() const noexcept { return root_; }

    std::shared_ptr<const SessionRepository> sessions() const {
        return repository;
    }

    web::SessionOpener opener() const {
        return [repository = repository, providers = providers](
                   const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            return open_session(
                *repository, identity, *providers, std::move(notifier));
        };
    }

    static web::InitialSelection initial_selection() {
        return {{std::string(entrance_id), std::string(welcome_id)}};
    }

    static FullSessionId welcome() {
        return {std::string(entrance_id), std::string(welcome_id)};
    }

private:
    std::filesystem::path root_;

public:
    std::shared_ptr<Providers> providers;
    std::shared_ptr<const SessionRepository> repository;
};

} // namespace cha::test
