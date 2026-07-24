#pragma once

#include "agents/agent_definition.h"
#include "agents/agent_protocol.h"
#include "agents/agent_roster.h"
#include "agents/completion_backend.h"
#include "conversation/conversation.h"
#include "agents/event_channel.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace cha {

class AgentRegistry {
public:
    AgentRegistry(const Conversation& conversation, std::vector<AgentDefinition> definitions);
    AgentRegistry(const Conversation& conversation, std::vector<std::unique_ptr<CompletionBackend>> backends);
    ~AgentRegistry() noexcept;

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    [[nodiscard]] const AgentRoster& roster() const noexcept;
    [[nodiscard]] bool submit(CompletionRequest request);
    void cancel();
    [[nodiscard]] ChannelReadStatus try_receive(AgentEvent& event);
    [[nodiscard]] int notification_fd() const;
    void stop();

private:
    struct WorkItem {
        std::size_t backend_index{};
        CompletionRequest request;
    };

    void dialog();
    void publish_event(AgentEvent event);
    void publish_terminal(AgentEvent event);

    const Conversation& conversation_;
    std::vector<std::unique_ptr<CompletionBackend>> backends_;
    AgentRoster roster_;
    EventChannel<WorkItem> requests_;
    AgentEventChannel events_;
    std::atomic_bool cancellation_{false};
    std::atomic_bool request_outstanding_{false};
    std::thread thread_;
    bool stopped_{};
};

} // namespace cha
