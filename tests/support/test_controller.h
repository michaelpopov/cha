#pragma once

#include "session/session_controller.h"

#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

namespace cha::test {

inline UserRoster operator_roster() {
    return {{.id = "operator", .display_name = "Operator"}};
}

inline std::unique_ptr<SessionController> from_definitions_for_testing(
    std::vector<AgentDefinition> definitions,
    UserRoster users,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {}) {
    return SessionController::from_definitions_for_testing(
        std::move(definitions),
        std::move(users),
        std::move(database_path),
        notifier,
        std::move(restored));
}

inline std::unique_ptr<SessionController> from_definitions_for_testing(
    std::vector<AgentDefinition> definitions,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {}) {
    return from_definitions_for_testing(
        std::move(definitions),
        operator_roster(),
        std::move(database_path),
        notifier,
        std::move(restored));
}

inline std::unique_ptr<SessionController> from_backends_for_testing(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    UserRoster users,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    SessionController::ActivationHook before_activation = {}) {
    return SessionController::from_backends_for_testing(
        std::move(backends),
        std::move(users),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(before_activation));
}

inline std::unique_ptr<SessionController> from_backends_for_testing(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    SessionController::ActivationHook before_activation = {}) {
    return from_backends_for_testing(
        std::move(backends),
        operator_roster(),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(before_activation));
}

} // namespace cha::test
