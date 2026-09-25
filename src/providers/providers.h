#pragma once

#include "providers/provider_client.h"
#include "providers/jev.h"
#include "providers/web_search.h"
#include "util/concurrent_queue.h"
#include "util/wake_notifier.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace cha {

// Complete owning input for one independently executed provider request.
struct ProviderRequestInput {
    SharedCharacterDefinition character;
    GenerationRequest generation;
    std::shared_ptr<WebSearchContext> web_search;
};

// Launches one detached provider worker. Tests can replace this only to make
// thread-start failure deterministic.
using ProviderThreadLauncher = std::function<void(std::function<void()>)>;

// The consumer handle for one request-owned provider execution.
class ProviderRequest final {
public:
    [[nodiscard]] const RunSpec& run() const noexcept;
    ChannelReadStatus try_receive(GenerationEvent& event);
    void cancel() noexcept;

private:
    friend class Providers;

    ProviderRequest(
        ProviderRequestInput input,
        std::shared_ptr<WakeNotifier> notifier);

    [[nodiscard]] bool has_valid_input() const noexcept;
    [[nodiscard]] std::string log_fields() const;
    void set_token(std::uint64_t token) noexcept;
    void execute(const ProviderClientFactory& client_factory,
        const WebSearchExecutor& web_search_executor) noexcept;
    void fail(std::string_view message) noexcept;
    void close_with(GenerationEvent event) noexcept;

    ProviderRequestInput input_;
    ConcurrentQueue<GenerationEvent> events_;
    std::atomic_bool cancellation_{false};
    std::shared_ptr<WakeNotifier> notifier_;
    std::uint64_t token_{};
};

// Process-owned supervision for independent request workers. It deliberately
// stores no provider configuration, credential, client, or scheduling state.
class Providers final {
public:
    Providers(
        ProviderClientFactory client_factory = {},
        ProviderThreadLauncher thread_launcher = {},
        JevExecutor jev_executor = {},
        WebSearchExecutor web_search_executor = {});
    ~Providers();

    Providers(const Providers&) = delete;
    Providers& operator=(const Providers&) = delete;

    [[nodiscard]] std::shared_ptr<ProviderRequest> make_request(
        ProviderRequestInput input,
        std::shared_ptr<WakeNotifier> notifier);
    [[nodiscard]] std::shared_ptr<JevRequest> make_jev_request(
        JevRequestInput input, std::shared_ptr<WakeNotifier> notifier);
    void shutdown() noexcept;
    [[nodiscard]] bool shutdown_until(
        std::chrono::steady_clock::time_point deadline) noexcept;

private:
    struct Registry;

    JevExecutor jev_executor_;
    WebSearchExecutor web_search_executor_;
    ProviderClientFactory client_factory_;
    ProviderThreadLauncher thread_launcher_;
    std::shared_ptr<Registry> registry_;
};

} // namespace cha
