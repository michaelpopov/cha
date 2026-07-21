#pragma once

#include "agent_definition.h"
#include "agent_roster.h"
#include "agent_worker.h"

#include <memory>
#include <string_view>
#include <unordered_map>
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
    void cancel(std::string_view agent_id);
    void cancel_all();
    [[nodiscard]] ChannelReadStatus try_receive(AgentEvent& event);
    [[nodiscard]] int notification_fd() const;
    void stop();

private:
    AgentEventChannel events_;
    std::vector<std::unique_ptr<AgentWorker>> workers_;
    std::unordered_map<std::string, std::size_t> index_;
    AgentRoster roster_;
    bool stopped_{};
};

} // namespace cha
