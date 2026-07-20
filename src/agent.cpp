#include "agent.h"

#include "completion_client.h"

#include <optional>
#include <stdexcept>
#include <utility>

namespace cha {

Agent::Agent(
    AgentDefinition definition,
    std::atomic_bool& cancellation)
    : Agent(
          std::make_unique<CompletionClient>(
              std::move(definition),
              cancellation),
          cancellation) {
}

Agent::Agent(
    std::unique_ptr<CompletionBackend> client,
    std::atomic_bool& cancellation)
    : client_(std::move(client)),
      cancellation_(cancellation) {
    if (!client_) {
        throw std::invalid_argument(
            "Agent requires a completion backend");
    }
}

Agent::~Agent() {
    stop();
}

void Agent::start(
    CompletionRequestChannel& requests,
    AgentEventChannel& events) {
    const std::scoped_lock lock(lifecycle_mutex_);
    if (state_ == State::running) {
        throw std::logic_error("Agent is already running");
    }
    if (state_ == State::stopped) {
        throw std::logic_error("Agent cannot be restarted after it is stopped");
    }

    input_ = &requests;
    try {
        thread_ = std::thread(
            &Agent::dialog,
            this,
            std::ref(requests),
            std::ref(events));
        state_ = State::running;
    } catch (...) {
        input_ = nullptr;
        throw;
    }
}

void Agent::stop() {
    const std::scoped_lock lock(lifecycle_mutex_);
    if (state_ == State::stopped) {
        return;
    }
    if (state_ == State::running) {
        // Borrow the caller-owned flag to interrupt the client, then restore it for
        // another agent or coordinator that may reuse the same cancellation state.
        const bool was_cancelled =
            cancellation_.exchange(true, std::memory_order_acq_rel);
        input_->close();
        thread_.join();
        cancellation_.store(was_cancelled, std::memory_order_release);
    }
    input_ = nullptr;
    state_ = State::stopped;
}

void Agent::dialog(
    CompletionRequestChannel& requests,
    AgentEventChannel& events) {
    while (true) {
        const std::optional<CompletionRequest> request = requests.get();
        if (!request) {
            break;
        }
        try {
            validate_completion_request(*request, client_->agent_id());
            if (cancellation_.load(std::memory_order_acquire)) {
                events.push(AgentCancelled{request->request_id});
                continue;
            }

            const CompletionResult result = client_->complete(
                *request,
                [&events, request_id = request->request_id](std::string text) {
                    events.push(AgentDelta{request_id, std::move(text)});
                });
            if (result.outcome == CompletionOutcome::cancelled) {
                events.push(AgentCancelled{request->request_id});
            } else if (result.outcome == CompletionOutcome::completed) {
                events.push(AgentCompleted{request->request_id});
            } else {
                events.push(AgentFailed{
                    request->request_id,
                    result.message,
                });
            }
        } catch (const std::exception& error) {
            events.push(AgentFailed{request->request_id, error.what()});
        }
    }
}

AgentInfo Agent::info() const {
    return client_->info();
}

} // namespace cha
