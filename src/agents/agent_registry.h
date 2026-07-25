#pragma once

#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "transcript/transcript.h"
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

// The outcome of resolving a handle the user typed: the agent when the match is unique, or the
// candidates that made it ambiguous, so the caller can explain the choice back to the user.
struct HandleResolution {
    HandleMatch match{HandleMatch::unknown};
    const AgentInfo* agent{};
    std::vector<const AgentInfo*> candidates;
};

// The ordered set of agents taking part in one room. It exists so the rest of the system can rely
// on a roster being non-empty and free of duplicate IDs or case-folded names: it validates that
// once at construction, then serves lookups by ID and resolution of user-typed handles over the
// AgentInfo values it holds.
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

// Runs agent completions off the caller's thread so the UI never blocks on a provider.
// It owns one CompletionBackend per roster entry and one worker thread, accepts a single
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

    const Transcript& transcript_;
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
