#pragma once

#include "agent_definition.h"
#include "agent_info.h"
#include "agent_protocol.h"
#include "completion_backend.h"

#include <atomic>
#include <memory>
#include <thread>

namespace cha {

// Runs one construction-started completion thread and owns its channels and cancellation token.
class AgentWorker {
public:
    explicit AgentWorker(AgentDefinition definition);
    // Accepts a completion backend for isolated worker tests or alternate transports.
    explicit AgentWorker(std::unique_ptr<CompletionBackend> client);
    ~AgentWorker() noexcept;

    AgentWorker(const AgentWorker&) = delete;
    AgentWorker& operator=(const AgentWorker&) = delete;

    // Queues one request, returning false while another request is outstanding or after stop.
    [[nodiscard]] bool submit(CompletionRequest request);
    void cancel();
    [[nodiscard]] ChannelReadStatus try_receive(AgentEvent& event);
    [[nodiscard]] int notification_fd() const;
    [[nodiscard]] AgentInfo info() const;
    // Cancels, unblocks, and joins the worker; repeated calls are harmless.
    void stop();

private:
    void dialog();
    void publish_terminal(AgentEvent event);

    std::atomic_bool cancellation_{false};
    std::atomic_bool request_outstanding_{false};
    std::unique_ptr<CompletionBackend> client_;
    CompletionRequestChannel requests_;
    AgentEventChannel events_;
    std::thread thread_;
    bool stopped_{};
};

} // namespace cha
