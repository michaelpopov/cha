#pragma once

#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "transcript/transcript.h"
#include "util/event_channel.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace cha {

// Runs agent completions off the caller's thread so the UI never blocks on a provider.
// It owns one CompletionBackend per room persona and one worker thread, accepts a single
// outstanding CompletionRequest, prepares that request from a short-lived Transcript read view,
// and publishes correlated AgentEvent values on a channel whose descriptor callers can poll.
// Cancellation is cooperative, and every accepted request receives a terminal event, including
// across shutdown.
class AgentRegistry {
public:
    AgentRegistry(const Transcript& transcript, std::vector<AgentDefinition> definitions);
    AgentRegistry(const Transcript& transcript, std::vector<std::unique_ptr<CompletionBackend>> backends);
    ~AgentRegistry() noexcept;

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    const std::vector<AgentRuntimeInfo>& runtime_info() const noexcept;
    // False means the request was not accepted (busy/stopped).
    [[nodiscard]] bool submit(CompletionRequest request);
    void cancel();
    [[nodiscard]] ChannelReadStatus try_receive(AgentEvent& event);
    int notification_fd() const;
    void stop();

private:
    struct WorkItem {
        std::size_t backend_index{};
        CompletionRequest request;
    };

    void dialog();
    void publish_event(AgentEvent event);
    void publish_terminal(AgentEvent event);

    const Transcript& transcript_;
    std::vector<std::unique_ptr<CompletionBackend>> backends_;
    std::vector<AgentRuntimeInfo> runtime_info_;
    EventChannel<WorkItem> requests_;
    AgentEventChannel events_;
    std::atomic_bool cancellation_{false};
    std::atomic_bool request_outstanding_{false};
    std::thread thread_;
    bool stopped_{};
};

} // namespace cha
