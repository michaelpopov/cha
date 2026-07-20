#pragma once

#include "agent_definition.h"
#include "agent_info.h"
#include "agent_protocol.h"
#include "completion_backend.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace cha {

// Runs one completion worker and translates immutable requests into identified events.
class Agent {
public:
    Agent(
        AgentDefinition definition,
        std::atomic_bool& cancellation);
    // Accepts a completion backend for isolated worker tests or alternate transports.
    Agent(
        std::unique_ptr<CompletionBackend> client,
        std::atomic_bool& cancellation);
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // Starts this agent's worker exactly once.
    void start(CompletionRequestChannel& requests, AgentEventChannel& events);
    [[nodiscard]] AgentInfo info() const;
    // Unblocks and joins the worker; repeated calls are harmless.
    void stop();

private:
    // Tracks the enforced one-shot worker lifecycle.
    enum class State {
        ready,
        running,
        stopped,
    };

    void dialog(CompletionRequestChannel& requests, AgentEventChannel& events);

    std::unique_ptr<CompletionBackend> client_;
    std::atomic_bool& cancellation_;
    std::thread thread_;
    CompletionRequestChannel* input_{};
    std::mutex lifecycle_mutex_;
    State state_{State::ready};
};

} // namespace cha
