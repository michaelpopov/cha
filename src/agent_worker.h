#pragma once

#include "agent_definition.h"
#include "agent_info.h"
#include "agent_protocol.h"
#include "completion_backend.h"
#include "conversation.h"

#include <atomic>
#include <memory>
#include <thread>

namespace cha {

// Runs one construction-started completion thread using the registry-owned event channel.
class AgentWorker {
public:
    AgentWorker(const Conversation& conversation, AgentEventChannel& events, AgentDefinition definition);
    AgentWorker(
        const Conversation& conversation,
        AgentEventChannel& events,
        std::unique_ptr<CompletionBackend> client);
    ~AgentWorker() noexcept;

    AgentWorker(const AgentWorker&) = delete;
    AgentWorker& operator=(const AgentWorker&) = delete;

    // Queues one request, returning false while another request is outstanding or after stop.
    [[nodiscard]] bool submit(CompletionRequest request);
    void cancel();
    [[nodiscard]] AgentInfo info() const;
    // Cancels, unblocks, and joins the worker; repeated calls are harmless.
    void stop();

private:
    void dialog();
    void publish_terminal(AgentEvent event);

    std::atomic_bool cancellation_{false};
    std::atomic_bool request_outstanding_{false};
    const Conversation& conversation_;
    std::unique_ptr<CompletionBackend> client_;
    CompletionRequestChannel requests_;
    AgentEventChannel* events_{};
    std::thread thread_;
    bool stopped_{};
};

} // namespace cha
