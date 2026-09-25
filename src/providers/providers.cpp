#include "providers/providers.h"

#include "util/logging.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
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

std::string token_usage_fields(const GenerationTokenUsage& usage) {
    const auto count = [](std::optional<std::uint64_t> value) {
        return value ? std::to_string(*value) : std::string("unreported");
    };
    const std::optional<std::uint64_t> total =
        usage.input_tokens && usage.output_tokens
            ? std::optional(*usage.input_tokens + *usage.output_tokens)
            : std::nullopt;
    return " input_tokens=" + count(usage.input_tokens)
        + " output_tokens=" + count(usage.output_tokens)
        + " total_tokens=" + count(total)
        + " cache_read_tokens=" + count(usage.cache_read_tokens)
        + " cache_write_tokens=" + count(usage.cache_write_tokens);
}

} // namespace

struct Providers::Registry {
    std::mutex mutex;
    std::condition_variable empty;
    bool admitting{true};
    using Request = std::variant<std::shared_ptr<ProviderRequest>, std::shared_ptr<JevRequest>>;
    std::unordered_map<std::uint64_t, Request> active;
    std::size_t diagnostic_tails{};
    std::uint64_t next_token{1};
};

ProviderRequest::ProviderRequest(
    ProviderRequestInput input,
    std::shared_ptr<WakeNotifier> notifier)
    : input_(std::move(input)),
      notifier_(std::move(notifier)) {
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
    notifier_->wake();
}

bool ProviderRequest::has_valid_input() const noexcept {
    if (!input_.character || !input_.generation.history) {
        return false;
    }
    return !input_.character->provider.id.empty()
        && !input_.generation.run.target.id.empty()
        && !input_.generation.run.target.display_name.empty();
}

std::string ProviderRequest::log_fields() const {
    const CharacterDefinition& character = *input_.character;
    const RunSpec& run = input_.generation.run;
    return "provider_id=" + character.provider.id
        + " character_id=" + run.target.id
        + " forum_id=" + run.session.forum_id
        + " session_id=" + run.session.session_id
        + " request_id=" + std::to_string(run.request_id)
        + " token=" + std::to_string(token_)
        + " model=" + character.provider.config.model;
}

void ProviderRequest::set_token(std::uint64_t token) noexcept {
    token_ = token;
}

void ProviderRequest::close_with(GenerationEvent event) noexcept {
    events_.close_with(std::move(event));
    notifier_->wake();
}

void ProviderRequest::fail(std::string_view message) noexcept {
    close_with(GenerationFailed{
        input_.generation.run.request_id,
        std::string(message),
    });
}

void ProviderRequest::execute(
    const ProviderClientFactory& client_factory,
    const WebSearchExecutor& web_search_executor) noexcept {
    const RequestId request_id = input_.generation.run.request_id;
    const auto started = std::chrono::steady_clock::now();
    std::string fields;

    try {
        fields = log_fields();
        if (cancellation_.load(std::memory_order_acquire)) {
            log_info("Provider request cancelled before client construction: " + fields);
            close_with(GenerationCancelled{request_id});
            return;
        }

        log_info("Provider request started: " + fields);
        auto generation = input_.generation;
        if (input_.web_search) {
            generation.web_search_context = input_.web_search->get(
                client_factory, web_search_executor, cancellation_);
        }
        if (cancellation_.load(std::memory_order_acquire)) {
            close_with(GenerationCancelled{request_id});
            return;
        }
        std::unique_ptr<ModelBackend> backend = client_factory(input_.character);
        if (!backend) {
            throw std::runtime_error("Provider client factory returned a null model backend");
        }
        if (cancellation_.load(std::memory_order_acquire)) {
            log_info("Provider request cancelled before preparation: " + fields);
            close_with(GenerationCancelled{request_id});
            return;
        }

        RequestPayload payload = backend->prepare(generation);
        fields += " request_payload_bytes=" + std::to_string(payload.bytes.size());
        if (payload.text_sizes) {
            fields += " system_prompt_bytes="
                + std::to_string(payload.text_sizes->system_prompt_bytes)
                + " conversation_bytes="
                + std::to_string(payload.text_sizes->conversation_bytes);
        } else {
            fields += " system_prompt_bytes=unreported conversation_bytes=unreported";
        }
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
                notifier_->wake();
            },
            cancellation_);

        if (result.outcome == GenerationOutcome::completed) {
            log_info("Provider request completed: " + fields
                + token_usage_fields(result.usage));
            close_with(GenerationCompleted{
                request_id,
                result.usage.input_tokens,
                result.usage.output_tokens,
            });
        } else if (result.outcome == GenerationOutcome::cancelled) {
            log_info("Provider request cancelled: " + fields
                + token_usage_fields(result.usage));
            close_with(GenerationCancelled{
                request_id,
                result.usage.input_tokens,
                result.usage.output_tokens,
            });
        } else {
            log_info("Provider request failed usage: " + fields
                + token_usage_fields(result.usage));
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
    ProviderThreadLauncher thread_launcher,
    JevExecutor jev_executor,
    WebSearchExecutor web_search_executor)
    : jev_executor_(std::move(jev_executor)),
      web_search_executor_(std::move(web_search_executor)),
      client_factory_(client_factory ? std::move(client_factory)
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
    if (!notifier) {
        throw std::invalid_argument("Provider request requires a notifier");
    }
    auto request = std::shared_ptr<ProviderRequest>(new ProviderRequest(
        std::move(input), std::move(notifier)));
    if (!request->has_valid_input()) {
        log_error("Provider request rejected: invalid input");
        request->fail("Provider request has invalid input");
        return request;
    }

    const std::shared_ptr<Registry> registry = registry_;
    ProviderClientFactory client_factory = client_factory_;
    WebSearchExecutor web_search_executor = web_search_executor_;

    std::unique_lock lock(registry->mutex);
    if (!registry->admitting) {
        lock.unlock();
        log_error("Provider request rejected: admission closed: "
            + request->log_fields());
        request->fail("Provider request admission is closed");
        return request;
    }

    const std::uint64_t token = registry->next_token++;
    request->set_token(token);
    registry->active.emplace(token, request);
    const std::size_t active_count = registry->active.size();
    // The active entry is the admission decision. Releasing this lock before
    // thread construction lets shutdown cancel and wait for the registered
    // request; the worker then observes cancellation before client creation.
    lock.unlock();
    const auto fail_launch = [&](std::string_view message) {
        request->fail(message);
        {
            std::lock_guard registry_lock(registry->mutex);
            registry->active.erase(token);
        }
        registry->empty.notify_all();
        log_error("Provider request failed to start: "
            + request->log_fields());
    };
    try {
        log_info("Provider request admitted: "
            + request->log_fields()
            + " active_count=" + std::to_string(active_count));
        thread_launcher_([registry, request, client_factory, web_search_executor, token]() mutable {
            request->execute(client_factory, web_search_executor);
            // The closure's factory copy may own test or transport support
            // state. Release it before unregistering so the detached tail
            // contains only its request, registry, and scalar token.
            client_factory = nullptr;
            web_search_executor = nullptr;

            std::size_t active_count;
            {
                std::lock_guard lock(registry->mutex);
                const auto found = registry->active.find(token);
                if (found == registry->active.end()) return;
                registry->active.erase(found);
                ++registry->diagnostic_tails;
                active_count = registry->active.size();
            }
            log_debug("Provider request unregistering: "
                + request->log_fields()
                + " active_count=" + std::to_string(active_count));
            {
                std::lock_guard lock(registry->mutex);
                --registry->diagnostic_tails;
            }
            registry->empty.notify_all();
        });
    } catch (const std::exception& error) {
        fail_launch(error.what());
    } catch (...) {
        fail_launch("Provider request worker could not be started");
    }
    return request;
}

std::shared_ptr<JevRequest> Providers::make_jev_request(
    JevRequestInput input, std::shared_ptr<WakeNotifier> notifier) {
    if (!notifier) throw std::invalid_argument("Jev request requires a notifier");
    input.deadline = std::min(input.deadline,
        std::chrono::steady_clock::now() + jev_request_timeout);
    auto request = std::shared_ptr<JevRequest>(new JevRequest(std::move(input), std::move(notifier)));
    try {
        validate_jev_config(request->input_.config);
        if (request->input_.prompt.empty() || request->input_.characters.empty())
            throw std::invalid_argument("Invalid Jev input");
        std::unordered_set<std::string> keys, ids;
        for (const auto& option : request->input_.characters) {
            if (option.key.empty() || option.key == "undefined" || option.key == "all_characters"
                || option.character_id.empty() || option.character_id == "*" || option.character_id == "-"
                || option.display_name.empty() || !keys.insert(option.key).second
                || !ids.insert(option.character_id).second) {
                throw std::invalid_argument("Invalid Jev option mapping");
            }
        }
    } catch (...) {
        request->publish({JevOutcome::failure, {}, "Invalid recipient detection input"});
        return request;
    }
    auto registry = registry_;
    auto executor = jev_executor_;
    std::unique_lock lock(registry->mutex);
    if (!registry->admitting) {
        lock.unlock();
        request->publish({JevOutcome::failure, {}, "Provider request admission is closed"});
        return request;
    }
    const auto token = registry->next_token++;
    registry->active.emplace(token, request);
    lock.unlock();
    try {
        thread_launcher_([registry, request, executor = std::move(executor), token]() mutable {
            request->execute(executor);
            executor = nullptr;
            {
                std::lock_guard lock(registry->mutex);
                registry->active.erase(token);
                ++registry->diagnostic_tails;
            }
            log_debug("Recipient detection worker finished token=" + std::to_string(token));
            {
                std::lock_guard lock(registry->mutex);
                --registry->diagnostic_tails;
            }
            registry->empty.notify_all();
        });
    } catch (...) {
        executor = nullptr;
        request->publish({JevOutcome::failure, {}, "Recipient detection worker could not be started"});
        {
            std::lock_guard lock(registry->mutex);
            registry->active.erase(token);
        }
        registry->empty.notify_all();
    }
    return request;
}

void Providers::shutdown() noexcept {
    const std::shared_ptr<Registry> registry = registry_;
    std::vector<Registry::Request> active;
    {
        std::lock_guard lock(registry->mutex);
        registry->admitting = false;
        active.reserve(registry->active.size());
        for (const auto& [_, request] : registry->active) {
            active.push_back(request);
        }
    }
    for (const auto& request : active) {
        std::visit([](const auto& value) { value->cancel(); }, request);
    }

    std::unique_lock lock(registry->mutex);
    registry->empty.wait(lock, [registry] {
        return registry->active.empty() && registry->diagnostic_tails == 0;
    });
}

bool Providers::shutdown_until(
    std::chrono::steady_clock::time_point deadline) noexcept {
    const std::shared_ptr<Registry> registry = registry_;
    std::vector<Registry::Request> active;
    {
        std::lock_guard lock(registry->mutex);
        registry->admitting = false;
        active.reserve(registry->active.size());
        for (const auto& [_, request] : registry->active) {
            active.push_back(request);
        }
    }
    for (const auto& request : active) {
        std::visit([](const auto& value) { value->cancel(); }, request);
    }

    std::unique_lock lock(registry->mutex);
    return registry->empty.wait_until(lock, deadline, [registry] {
        return registry->active.empty() && registry->diagnostic_tails == 0;
    });
}

} // namespace cha
