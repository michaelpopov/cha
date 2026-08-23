#pragma once

#include "session/session_controller.h"
#include "support/test_backends.h"
#include "support/test_workspace.h"

#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cha::test {

// Publishes a small real Workspace, then constructs the controller through the
// same Workspace path as production. Only provider behavior and failure hooks
// are injected.
class TestController {
public:
    TestController(
        std::vector<CharacterDefinition> definitions,
        PersonaRoster personas,
        ParticipantId default_character_id,
        std::filesystem::path database_path,
        std::shared_ptr<WakeNotifier> notifier,
        SessionRestore restored = {},
        ProviderClientFactory client_factory = {},
        ProviderThreadLauncher thread_launcher = {},
        SessionController::ActivationHook before_activation = {},
        std::vector<TestWorkspaceStyle> styles = {},
        SessionIdentity identity = {},
        bool reuse_current_workspace = false)
        : notifier_(std::move(notifier)),
          workspace_(publish_test_workspace(
              definitions, personas,
              definitions.empty() ? std::string_view{}
                                  : std::string_view(definitions.front().character.id),
              database_path,
              std::move(identity), styles, reuse_current_workspace)),
          controller_(SessionController::from_workspace_for_testing(
              std::move(default_character_id), workspace_.default_persona_id,
              std::move(database_path),
              SessionLease::inactive_for_testing(),
              std::make_shared<Providers>(
                  std::move(client_factory), std::move(thread_launcher)),
              notifier_, std::move(restored),
              std::move(before_activation),
              workspace_.identity)) {}

    ~TestController() {
        controller_.reset();
    }
    TestController(TestController&&) = delete;
    TestController& operator=(TestController&&) = delete;
    TestController(const TestController&) = delete;
    TestController& operator=(const TestController&) = delete;

    [[nodiscard]] SessionController* operator->() const noexcept { return controller_.get(); }
    [[nodiscard]] SessionController& operator*() const noexcept { return *controller_; }
    [[nodiscard]] std::unique_ptr<SessionController> release() && {
        return std::move(controller_);
    }

private:
    std::shared_ptr<WakeNotifier> notifier_;
    PublishedTestWorkspace workspace_;
    std::unique_ptr<SessionController> controller_;
};

inline ControllerUpdate receive_all_events(SessionController& controller) {
    return std::move(
        controller.receive_events(std::numeric_limits<std::size_t>::max()).update);
}

inline PersonaRoster operator_roster() {
    return {{.id = "operator", .display_name = "Operator"}};
}

// Some controller tests coordinate several calls through one shared scenario.
// Each factory call still returns a distinct, unsynchronized backend facade,
// matching production request-local ownership and overlap.
class RequestBackendFacade final : public ModelBackend {
public:
    struct Slot {
        explicit Slot(std::unique_ptr<DescribedModelBackend> value)
            : scenario(std::move(value)) {}
        std::unique_ptr<DescribedModelBackend> scenario;
    };

    explicit RequestBackendFacade(std::shared_ptr<Slot> slot) : slot_(std::move(slot)) {}

    RequestPayload prepare(const GenerationRequest& input) override {
        return slot_->scenario->prepare(input);
    }

    GenerationResult perform(
        RequestPayload payload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        return slot_->scenario->perform(std::move(payload), on_delta, cancellation);
    }

private:
    std::shared_ptr<Slot> slot_;
};

inline TestController from_test_workspace(
    std::vector<CharacterDefinition> definitions,
    PersonaRoster personas,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt,
    ProviderThreadLauncher thread_launcher = {}) {
    const ParticipantId default_character_id = initial_default_character_id.value_or(
        definitions.empty() ? CharacterId{} : definitions.front().character.id);
    return TestController(
        std::move(definitions),
        std::move(personas),
        default_character_id,
        std::move(database_path),
        std::shared_ptr<WakeNotifier>(&notifier, [](WakeNotifier*) {}),
        std::move(restored), {}, std::move(thread_launcher));
}

inline TestController from_test_workspace(
    std::vector<CharacterDefinition> definitions,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt,
    ProviderThreadLauncher thread_launcher = {}) {
    return from_test_workspace(
        std::move(definitions),
        operator_roster(),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(initial_default_character_id),
        std::move(thread_launcher));
}

inline TestController from_test_backends(
    std::vector<std::unique_ptr<DescribedModelBackend>> backends,
    PersonaRoster personas,
    std::filesystem::path database_path,
    std::shared_ptr<WakeNotifier> notifier,
    SessionRestore restored = {},
    SessionController::ActivationHook before_activation = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt,
    SessionIdentity identity = {},
    bool reuse_current_workspace = false) {
    std::vector<CharacterDefinition> definitions;
    definitions.reserve(backends.size());
    for (const std::unique_ptr<DescribedModelBackend>& backend : backends) {
        if (!backend) throw std::invalid_argument("Test controller requires model backends");
        const DescribedBackendInfo info = backend->info();
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
    using Slot = RequestBackendFacade::Slot;
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
        return std::make_unique<RequestBackendFacade>(found->second);
    };
    return TestController(
        std::move(definitions),
        std::move(personas),
        default_character_id,
        std::move(database_path),
        std::move(notifier),
        std::move(restored),
        std::move(factory),
        {},
        std::move(before_activation),
        {},
        std::move(identity),
        reuse_current_workspace);
}

inline TestController from_test_backends(
    std::vector<std::unique_ptr<DescribedModelBackend>> backends,
    PersonaRoster personas,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    SessionController::ActivationHook before_activation = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt,
    SessionIdentity identity = {},
    bool reuse_current_workspace = false) {
    return from_test_backends(
        std::move(backends),
        std::move(personas),
        std::move(database_path),
        std::shared_ptr<WakeNotifier>(&notifier, [](WakeNotifier*) {}),
        std::move(restored),
        std::move(before_activation),
        std::move(initial_default_character_id),
        std::move(identity),
        reuse_current_workspace);
}

inline TestController from_test_backends(
    std::vector<std::unique_ptr<DescribedModelBackend>> backends,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored = {},
    SessionController::ActivationHook before_activation = {},
    std::optional<ParticipantId> initial_default_character_id = std::nullopt,
    SessionIdentity identity = {},
    bool reuse_current_workspace = false) {
    return from_test_backends(
        std::move(backends),
        operator_roster(),
        std::move(database_path),
        std::shared_ptr<WakeNotifier>(&notifier, [](WakeNotifier*) {}),
        std::move(restored),
        std::move(before_activation),
        std::move(initial_default_character_id),
        std::move(identity),
        reuse_current_workspace);
}

} // namespace cha::test
