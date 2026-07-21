#include "agent_worker.h"

#include "completion_client.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cha {

AgentWorker::AgentWorker(
    const Conversation& conversation,
    AgentEventChannel& events,
    AgentDefinition definition)
    : AgentWorker(
          conversation,
          events,
          std::make_unique<CompletionClient>(
              std::move(definition))) {
}

AgentWorker::AgentWorker(
    const Conversation& conversation,
    AgentEventChannel& events,
    std::unique_ptr<CompletionBackend> client)
    : conversation_(conversation),
      client_(std::move(client)),
      events_(&events) {
    if (!client_) {
        throw std::invalid_argument(
            "Agent worker requires a completion backend");
    }
    thread_ = std::thread(&AgentWorker::dialog, this);
}

AgentWorker::~AgentWorker() noexcept {
    try {
        stop();
    } catch (...) {
        // If shutdown failed after spawning the thread, make one final join attempt
        // so destruction never leaks an exception into std::thread's destructor.
        if (thread_.joinable()) {
            try {
                thread_.join();
            } catch (...) {
                std::terminate();
            }
        }
    }
}

bool AgentWorker::submit(CompletionRequest request) {
    if (stopped_) {
        return false;
    }

    bool expected = false;
    if (!request_outstanding_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel)) {
        return false;
    }

    cancellation_.store(false, std::memory_order_release);
    try {
        if (requests_.push(std::move(request))) {
            return true;
        }
    } catch (...) {
        request_outstanding_.store(false, std::memory_order_release);
        throw;
    }
    request_outstanding_.store(false, std::memory_order_release);
    return false;
}

void AgentWorker::cancel() {
    cancellation_.store(true, std::memory_order_release);
}

void AgentWorker::stop() {
    if (stopped_) {
        return;
    }
    // A stopped worker cannot be restarted, so its cancellation token stays set.
    cancellation_.store(true, std::memory_order_release);
    requests_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
    stopped_ = true;
}

void AgentWorker::dialog() {
    while (true) {
        const std::optional<CompletionRequest> request = requests_.get();
        if (!request) {
            break;
        }
        try {
            validate_completion_request(*request, client_->agent_id());
            if (cancellation_.load(std::memory_order_acquire)) {
                publish_terminal(AgentCancelled{request->request_id});
                continue;
            }

            RequestPayload payload;
            {
                ConversationReadView conversation = conversation_.read();
                validate_completion_context(*request, conversation);
                payload = client_->prepare(*request, conversation);
            }
            const CompletionResult result = client_->perform(
                std::move(payload),
                [this, request_id = request->request_id](std::string text) {
                    events_->push(AgentDelta{request_id, std::move(text)});
                },
                cancellation_);
            if (result.outcome == CompletionOutcome::cancelled) {
                publish_terminal(
                    AgentCancelled{request->request_id});
            } else if (result.outcome == CompletionOutcome::completed) {
                publish_terminal(
                    AgentCompleted{request->request_id});
            } else {
                publish_terminal(AgentFailed{
                    request->request_id,
                    result.message,
                });
            }
        } catch (const std::exception& error) {
            publish_terminal(
                AgentFailed{request->request_id, error.what()});
        }
    }
}

void AgentWorker::publish_terminal(AgentEvent event) {
    request_outstanding_.store(false, std::memory_order_release);
    events_->push(std::move(event));
}

AgentInfo AgentWorker::info() const {
    return client_->info();
}

} // namespace cha
