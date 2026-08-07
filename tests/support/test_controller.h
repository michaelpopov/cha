#pragma once

#include "session/session_controller.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cha::test {

inline SessionChange receive_all_events(SessionController& controller) {
    return std::move(
        controller.receive_events(std::numeric_limits<std::size_t>::max()).change);
}

inline PersonaRoster operator_roster() {
    return {{.id = "operator", .display_name = "Operator"}};
}

inline std::unique_ptr<SessionController> from_definitions_for_testing(
    std::vector<AgentDefinition> definitions,
    PersonaRoster personas,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    std::optional<ParticipantId> initial_default_agent_id = std::nullopt) {
    const ParticipantId default_agent_id = initial_default_agent_id.value_or(
        definitions.empty() ? ParticipantId{} : definitions.front().config.id);
    return SessionController::from_definitions_for_testing(
        std::move(definitions),
        std::move(personas),
        default_agent_id,
        std::move(database_path),
        notifier,
        std::move(restored));
}

inline std::unique_ptr<SessionController> from_definitions_for_testing(
    std::vector<AgentDefinition> definitions,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    std::optional<ParticipantId> initial_default_agent_id = std::nullopt) {
    return from_definitions_for_testing(
        std::move(definitions),
        operator_roster(),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(initial_default_agent_id));
}

inline std::unique_ptr<SessionController> from_backends_for_testing(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    PersonaRoster personas,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    SessionController::ActivationHook before_activation = {},
    std::optional<ParticipantId> initial_default_agent_id = std::nullopt) {
    const ParticipantId default_agent_id = initial_default_agent_id.value_or(
        backends.empty() ? ParticipantId{} : backends.front()->info().character.id);
    return SessionController::from_backends_for_testing(
        std::move(backends),
        std::move(personas),
        default_agent_id,
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
    SessionController::ActivationHook before_activation = {},
    std::optional<ParticipantId> initial_default_agent_id = std::nullopt) {
    return from_backends_for_testing(
        std::move(backends),
        operator_roster(),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(before_activation),
        std::move(initial_default_agent_id));
}

} // namespace cha::test
