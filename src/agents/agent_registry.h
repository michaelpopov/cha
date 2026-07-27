#pragma once

#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "util/concurrent_queue.h"
#include "util/wake_notifier.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cha {

// Runs agent completions off the caller's thread so the UI never blocks on a provider.
// It owns one CompletionBackend per forum persona and one worker thread, accepts a single
// outstanding CompletionInput, prepares from its immutable history, and
// publishes correlated AgentEvent values on a portable queue. A front-end
// notifier wakes the event loop after event queue state changes.
// Cancellation is cooperative, and every accepted request receives a terminal event, including
// across shutdown.
class AgentRegistry {
public:
    AgentRegistry(
        std::vector<AgentDefinition> definitions,
        WakeNotifier& notifier);
    AgentRegistry(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        WakeNotifier& notifier);
    ~AgentRegistry() noexcept;

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    const std::vector<AgentRuntimeInfo>& runtime_info() const noexcept;
    // False means the request was not accepted (busy, stopped, unknown target,
    // or queue admission failure). Missing immutable history is a caller error.
    [[nodiscard]] bool submit(CompletionInput input);
    void cancel();
    [[nodiscard]] ChannelReadStatus try_receive(AgentEvent& event);
    void stop();

private:
    struct WorkItem {
        std::size_t backend_index{};
        CompletionInput input;
    };

    void dialog();
    void publish_event(AgentEvent event);
    void publish_terminal(AgentEvent event);

    std::vector<std::unique_ptr<CompletionBackend>> backends_;
    std::vector<AgentRuntimeInfo> runtime_info_;
    ConcurrentQueue<WorkItem> requests_;
    // Event mutation has exactly two funnels: publish_event() pushes then
    // wakes, and stop() closes then wakes. Keep every future mutation paired
    // with notifier_.wake() so a sleeping frontend can observe the new state.
    ConcurrentQueue<AgentEvent> events_;
    WakeNotifier& notifier_;
    std::atomic_bool cancellation_{false};
    std::atomic_bool request_outstanding_{false};
    std::thread thread_;
    bool stopped_{};
};

} // namespace cha
