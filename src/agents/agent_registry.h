#pragma once

#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "conversation/conversation.h"
#include "util/event_channel.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cha {

enum class HandleMatch { resolved, unknown, ambiguous };

struct HandleResolution {
    HandleMatch match{HandleMatch::unknown};
    const AgentInfo* agent{};
    std::vector<const AgentInfo*> candidates;
};

// Owns one non-empty ordered roster with unique IDs, unique case-folded names, and handle resolution.
class AgentRoster {
public:
    explicit AgentRoster(std::vector<AgentInfo> agents);

    const std::vector<AgentInfo>& agents() const noexcept;
    const AgentInfo& first() const;
    const AgentInfo* find(std::string_view id) const;
    HandleResolution resolve_handle(std::string_view handle) const;
    std::string handle_list() const;

private:
    std::vector<AgentInfo> agents_;
};

// Runs one execution thread that routes completion work to backends and publishes correlated agent events.
class AgentRegistry {
public:
    AgentRegistry(const Conversation& conversation, std::vector<AgentDefinition> definitions);
    AgentRegistry(const Conversation& conversation, std::vector<std::unique_ptr<CompletionBackend>> backends);
    ~AgentRegistry() noexcept;

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    const AgentRoster& roster() const noexcept;
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
