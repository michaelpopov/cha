#pragma once

#include "session/session_controller.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cha::test {

// Test counterpart of the production construction order. The notifier and
// immutable definitions outlive the controller, and destruction explicitly
// releases the controller first. Block 5 can add the process-owned Providers
// member ahead of these fields without changing callers.
class TestController {
public:
    TestController(
        std::vector<CharacterDefinition> definitions,
        PersonaRoster personas,
        ParticipantId default_character_id,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored = {},
        ProviderClientFactory client_factory = {},
        SessionController::ActivationHook before_activation = {},
        SessionController::StyleResolver style_resolver = {},
        SessionIdentity identity = {})
        : notifier_(&notifier, [](WakeNotifier*) {}),
          definitions_(share_character_definitions(std::move(definitions))),
          controller_(SessionController::from_definitions_for_testing(
              definitions_, std::move(personas), std::move(default_character_id),
              std::move(database_path), *notifier_, std::move(restored),
              std::move(client_factory), std::move(before_activation),
              std::move(style_resolver), std::move(identity))) {}

    ~TestController() { controller_.reset(); }
    TestController(TestController&&) noexcept = default;
    TestController& operator=(TestController&&) noexcept = default;
    TestController(const TestController&) = delete;
    TestController& operator=(const TestController&) = delete;

    [[nodiscard]] SessionController* operator->() const noexcept { return controller_.get(); }
    [[nodiscard]] SessionController& operator*() const noexcept { return *controller_; }
    [[nodiscard]] std::unique_ptr<SessionController> take_controller() && {
        return std::move(controller_);
    }

private:
    std::shared_ptr<WakeNotifier> notifier_;
    std::vector<SharedCharacterDefinition> definitions_;
    std::unique_ptr<SessionController> controller_;
};

inline ControllerUpdate receive_all_events(SessionController& controller) {
    return std::move(
        controller.receive_events(std::numeric_limits<std::size_t>::max()).update);
}

inline PersonaRoster operator_roster() {
    return {{.id = "operator", .display_name = "Operator"}};
}

// Old-controller tests often describe one character with a stateful fake.
// Keep that state in a separately shared slot and return a fresh request
// facade for every factory call. This already has the lifetime and locking
// shape that request-owned execution needs, while preserving the existing
// observations used by the old-executor tests.
class SharedBackendProxy final : public ModelBackend {
public:
    struct Slot {
        explicit Slot(std::unique_ptr<ModelBackend> value) : backend(std::move(value)) {}
        std::unique_ptr<ModelBackend> backend;
        std::mutex mutex;
    };

    explicit SharedBackendProxy(std::shared_ptr<Slot> slot) : slot_(std::move(slot)) {}

    RequestPayload prepare(const GenerationRequest& input) override {
        std::lock_guard lock(slot_->mutex);
        return slot_->backend->prepare(input);
    }

    GenerationResult perform(
        RequestPayload payload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        std::lock_guard lock(slot_->mutex);
        return slot_->backend->perform(std::move(payload), on_delta, cancellation);
    }

    ModelBackendInfo info() const override {
        std::lock_guard lock(slot_->mutex);
        return slot_->backend->info();
    }

private:
    std::shared_ptr<Slot> slot_;
};

inline TestController from_definitions_for_testing(
    std::vector<CharacterDefinition> definitions,
    PersonaRoster personas,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt) {
    const ParticipantId default_character_id = initial_default_character_id.value_or(
        definitions.empty() ? CharacterId{} : definitions.front().character.id);
    return TestController(
        std::move(definitions),
        std::move(personas),
        default_character_id,
        std::move(database_path),
        notifier,
        std::move(restored));
}

inline TestController from_definitions_for_testing(
    std::vector<CharacterDefinition> definitions,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt) {
    return from_definitions_for_testing(
        std::move(definitions),
        operator_roster(),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(initial_default_character_id));
}

inline TestController from_test_backends(
    std::vector<std::unique_ptr<ModelBackend>> backends,
    PersonaRoster personas,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    SessionController::ActivationHook before_activation = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt,
    SessionIdentity identity = {}) {
    std::vector<CharacterDefinition> definitions;
    definitions.reserve(backends.size());
    for (const std::unique_ptr<ModelBackend>& backend : backends) {
        if (!backend) throw std::invalid_argument("Test controller requires model backends");
        const ModelBackendInfo info = backend->info();
        definitions.push_back({
            .character = info.character,
            .provider = {.id = "test", .config = {
                .host = "127.0.0.1",
                .port = 1,
                .model = info.model,
                .stream = info.streaming,
            }},
            .system_prompt = "Test prompt",
        });
    }
    const ParticipantId default_character_id = initial_default_character_id.value_or(
        definitions.empty() ? ParticipantId{} : definitions.front().character.id);
    using Slot = SharedBackendProxy::Slot;
    auto supplied = std::make_shared<std::unordered_map<ParticipantId, std::shared_ptr<Slot>>>();
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        supplied->emplace(
            definitions[index].character.id,
            std::make_shared<Slot>(std::move(backends[index])));
    }
    ProviderClientFactory factory = [supplied](SharedCharacterDefinition definition) {
        const auto found = supplied->find(definition->character.id);
        if (found == supplied->end()) {
            throw std::runtime_error("Test backend factory received an unexpected character");
        }
        return std::make_unique<SharedBackendProxy>(found->second);
    };
    return TestController(
        std::move(definitions),
        std::move(personas),
        default_character_id,
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(factory),
        std::move(before_activation),
        {},
        std::move(identity));
}

inline TestController from_test_backends(
    std::vector<std::unique_ptr<ModelBackend>> backends,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    SessionController::ActivationHook before_activation = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt,
    SessionIdentity identity = {}) {
    return from_test_backends(
        std::move(backends),
        operator_roster(),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(before_activation),
        std::move(initial_default_character_id),
        std::move(identity));
}

} // namespace cha::test
