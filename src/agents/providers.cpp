#include "agents/providers.h"

#include "util/logging.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace cha {
namespace {

std::unique_ptr<ModelBackend> default_client_factory(
    SharedCharacterDefinition definition) {
    return std::make_unique<ProviderClient>(std::move(definition));
}

void default_thread_launcher(std::function<void()> worker) {
    std::thread(std::move(worker)).detach();
}

std::string lifecycle_fields(
    std::string_view provider_id,
    std::string_view model,
    RequestId request_id,
    std::uint64_t token) {
    return "provider_id=" + std::string(provider_id)
        + " request_id=" + std::to_string(request_id)
        + " token=" + std::to_string(token)
        + " model=" + std::string(model);
}

} // namespace

struct Providers::Registry {
    std::mutex mutex;
    std::condition_variable empty;
    bool admitting{true};
    std::unordered_map<std::uint64_t, std::shared_ptr<ProviderRequest>> active;
    std::uint64_t next_token{1};
};

ProviderRequest::ProviderRequest(
    ProviderRequestInput input,
    std::shared_ptr<WakeNotifier> notifier)
    : input_(std::move(input)),
      notifier_(std::move(notifier)),
      fallback_terminal_(GenerationFailed{
          input_.generation.run.request_id,
          "Provider request failed before details could be reported",
      }) {
    static_assert(std::is_nothrow_move_constructible_v<GenerationEvent>);
    static_assert(std::is_nothrow_move_assignable_v<GenerationEvent>);
}

const RunSpec& ProviderRequest::run() const noexcept {
    return input_.generation.run;
}

ChannelReadStatus ProviderRequest::try_receive(GenerationEvent& event) {
    return events_.try_get(event);
}

void ProviderRequest::cancel() noexcept {
    cancellation_.store(true, std::memory_order_release);
    if (notifier_) notifier_->wake();
}

bool ProviderRequest::has_valid_input() const noexcept {
    if (!input_.character || !input_.generation.history || !notifier_) {
        return false;
    }
    const CharacterDefinition& character = *input_.character;
    return !character.character.id.empty()
        && !character.character.display_name.empty()
        && !character.provider.id.empty()
        && !character.provider.config.model.empty()
        && !input_.generation.run.target.id.empty()
        && !input_.generation.run.target.display_name.empty();
}

void ProviderRequest::set_token(std::uint64_t token) noexcept {
    token_ = token;
}

void ProviderRequest::close_with(GenerationEvent event) noexcept {
    events_.close_with(std::move(event));
    if (notifier_) notifier_->wake();
}

void ProviderRequest::fail(std::string_view message) noexcept {
    GenerationEvent terminal = std::move(fallback_terminal_);
    try {
        terminal = GenerationFailed{
            input_.generation.run.request_id,
            std::string(message),
        };
    } catch (...) {
        log_critical("Provider request failure details could not be preserved");
    }
    close_with(std::move(terminal));
}

void ProviderRequest::execute(
    const ProviderClientFactory& client_factory) noexcept {
    const RequestId request_id = input_.generation.run.request_id;
    const CharacterDefinition& character = *input_.character;
    const auto started = std::chrono::steady_clock::now();
    std::string fields;

    try {
        fields = lifecycle_fields(
            character.provider.id,
            character.provider.config.model,
            request_id,
            token_);
        if (cancellation_.load(std::memory_order_acquire)) {
            log_info("Provider request cancelled before client construction: " + fields);
            close_with(GenerationCancelled{request_id});
            return;
        }

        log_info("Provider request started: " + fields);
        std::unique_ptr<ModelBackend> backend = client_factory(input_.character);
        if (!backend) {
            throw std::runtime_error("Provider client factory returned a null model backend");
        }
        if (cancellation_.load(std::memory_order_acquire)) {
            log_info("Provider request cancelled before preparation: " + fields);
            close_with(GenerationCancelled{request_id});
            return;
        }

        RequestPayload payload = backend->prepare(input_.generation);
        const GenerationResult result = backend->perform(
            std::move(payload),
            [this, request_id](GenerationDelta delta) {
                if (!events_.push(GenerationEventDelta{
                        request_id,
                        delta.kind,
                        std::move(delta.text),
                    })) {
                    throw std::logic_error(
                        "Provider request event queue closed before execution stopped");
                }
                if (notifier_) notifier_->wake();
            },
            cancellation_);

        if (result.outcome == GenerationOutcome::completed) {
            log_info("Provider request completed: " + fields);
            close_with(GenerationCompleted{request_id});
        } else if (result.outcome == GenerationOutcome::cancelled) {
            log_info("Provider request cancelled: " + fields);
            close_with(GenerationCancelled{request_id});
        } else {
            log_error("Provider request failed: " + fields);
            fail(result.message);
        }
    } catch (const std::exception& error) {
        log_error("Provider request raised an exception: " + fields);
        fail(error.what());
    } catch (...) {
        log_error("Provider request raised an unknown exception: " + fields);
        fail("Unknown provider request failure");
    }

    log_debug(
        "Provider request transport finished: " + fields
        + " duration_ms=" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count()));
}

Providers::Providers(
    ProviderClientFactory client_factory,
    ProviderThreadLauncher thread_launcher)
    : client_factory_(client_factory ? std::move(client_factory)
                                    : ProviderClientFactory(default_client_factory)),
      thread_launcher_(thread_launcher ? std::move(thread_launcher)
                                       : ProviderThreadLauncher(default_thread_launcher)),
      registry_(std::make_shared<Registry>()) {
}

Providers::~Providers() {
    shutdown();
}

std::shared_ptr<ProviderRequest> Providers::make_request(
    ProviderRequestInput input,
    std::shared_ptr<WakeNotifier> notifier) {
    auto request = std::shared_ptr<ProviderRequest>(new ProviderRequest(
        std::move(input), std::move(notifier)));
    if (!request->has_valid_input()) {
        log_error("Provider request rejected: invalid input");
        request->fail("Provider request has invalid input");
        return request;
    }

    const std::shared_ptr<Registry> registry = registry_;
    ProviderClientFactory client_factory = client_factory_;

    std::unique_lock lock(registry->mutex);
    if (!registry->admitting) {
        lock.unlock();
        const CharacterDefinition& character = *request->input_.character;
        log_error("Provider request rejected: admission closed: "
            + lifecycle_fields(
                character.provider.id,
                character.provider.config.model,
                request->input_.generation.run.request_id,
                0));
        request->fail("Provider request admission is closed");
        return request;
    }

    const std::uint64_t token = registry->next_token++;
    request->set_token(token);
    registry->active.emplace(token, request);
    // The active entry is the admission decision. Releasing this lock before
    // thread construction lets shutdown cancel and wait for the registered
    // request; the worker then observes cancellation before client creation.
    lock.unlock();
    try {
        thread_launcher_([registry, request, client_factory, token]() mutable {
            request->execute(client_factory);
            // The closure's factory copy may own test or transport support
            // state. Release it before unregistering so the detached tail
            // contains only its request, registry, and scalar token.
            client_factory = nullptr;

            std::unique_lock lock(registry->mutex);
            const auto found = registry->active.find(token);
            if (found != registry->active.end()) {
                log_debug("Provider request unregistering: token="
                    + std::to_string(token)
                    + " active_count=" + std::to_string(registry->active.size() - 1));
                registry->active.erase(found);
                lock.unlock();
                registry->empty.notify_all();
            }
        });
    } catch (const std::exception& error) {
        const CharacterDefinition& character = *request->input_.character;
        log_error("Provider request failed to start: "
            + lifecycle_fields(
                character.provider.id,
                character.provider.config.model,
                request->input_.generation.run.request_id,
                token));
        request->fail(error.what());
        {
            std::lock_guard registry_lock(registry->mutex);
            registry->active.erase(token);
        }
        registry->empty.notify_all();
    } catch (...) {
        const CharacterDefinition& character = *request->input_.character;
        log_error("Provider request failed to start: "
            + lifecycle_fields(
                character.provider.id,
                character.provider.config.model,
                request->input_.generation.run.request_id,
                token));
        request->fail("Provider request worker could not be started");
        {
            std::lock_guard registry_lock(registry->mutex);
            registry->active.erase(token);
        }
        registry->empty.notify_all();
    }
    return request;
}

void Providers::shutdown() noexcept {
    const std::shared_ptr<Registry> registry = registry_;
    std::vector<std::shared_ptr<ProviderRequest>> active;
    {
        std::lock_guard lock(registry->mutex);
        registry->admitting = false;
        active.reserve(registry->active.size());
        for (const auto& [_, request] : registry->active) {
            active.push_back(request);
        }
    }
    for (const std::shared_ptr<ProviderRequest>& request : active) {
        request->cancel();
    }

    std::unique_lock lock(registry->mutex);
    registry->empty.wait(lock, [registry] { return registry->active.empty(); });
}

} // namespace cha
