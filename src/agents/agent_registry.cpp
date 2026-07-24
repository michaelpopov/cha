#include "agents/agent_registry.h"

#include "agents/completion_client.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace cha {
namespace {

AgentRoster build_roster(
    const std::vector<std::unique_ptr<CompletionBackend>>& backends) {
    if (backends.empty()) {
        throw std::invalid_argument("Agent registry requires at least one agent");
    }
    std::vector<AgentInfo> infos;
    infos.reserve(backends.size());
    for (const auto& backend : backends) {
        if (!backend) {
            throw std::invalid_argument("Agent registry requires completion backends");
        }
        infos.push_back(backend->info());
    }
    return AgentRoster(std::move(infos));
}

std::vector<std::unique_ptr<CompletionBackend>> build_backends(
    std::vector<AgentDefinition> definitions) {
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.reserve(definitions.size());
    for (AgentDefinition& definition : definitions) {
        const std::string id = definition.config.id;
        const std::string name = definition.config.name;
        try {
            backends.push_back(
                std::make_unique<CompletionClient>(std::move(definition)));
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Persona '" + name + "' (agent ID '" + id
                + "') failed to initialize: " + error.what());
        }
    }
    return backends;
}

} // namespace

AgentRegistry::AgentRegistry(
    const Conversation& conversation,
    std::vector<AgentDefinition> definitions)
    : AgentRegistry(conversation, build_backends(std::move(definitions))) {
}

AgentRegistry::AgentRegistry(
    const Conversation& conversation,
    std::vector<std::unique_ptr<CompletionBackend>> backends)
    : conversation_(conversation),
      backends_(std::move(backends)),
      roster_(build_roster(backends_)),
      thread_(&AgentRegistry::dialog, this) {
}

AgentRegistry::~AgentRegistry() noexcept {
    try {
        stop();
    } catch (...) {
        if (thread_.joinable()) {
            try {
                thread_.join();
            } catch (...) {
                std::terminate();
            }
        }
    }
}

const AgentRoster& AgentRegistry::roster() const noexcept { return roster_; }

bool AgentRegistry::submit(CompletionRequest request) {
    if (stopped_) {
        return false;
    }

    const std::vector<AgentInfo>& agents = roster_.agents();
    std::size_t backend_index = agents.size();
    for (std::size_t index = 0; index < agents.size(); ++index) {
        if (agents[index].id == request.prompt.addressed_to) {
            backend_index = index;
            break;
        }
    }
    if (backend_index == agents.size()) {
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
        if (requests_.push(WorkItem{backend_index, std::move(request)})) {
            return true;
        }
    } catch (...) {
        request_outstanding_.store(false, std::memory_order_release);
        throw;
    }
    request_outstanding_.store(false, std::memory_order_release);
    return false;
}

void AgentRegistry::cancel() {
    cancellation_.store(true, std::memory_order_release);
}

ChannelReadStatus AgentRegistry::try_receive(AgentEvent& event) {
    return events_.try_get(event);
}

int AgentRegistry::notification_fd() const { return events_.notification_fd(); }

void AgentRegistry::stop() {
    if (stopped_) {
        return;
    }

    cancellation_.store(true, std::memory_order_release);
    requests_.close();
    if (thread_.joinable()) {
        thread_.join();
    }
    events_.close();
    stopped_ = true;
}

void AgentRegistry::dialog() {
    while (true) {
        const std::optional<WorkItem> work = requests_.get();
        if (!work) {
            break;
        }

        const RequestId request_id = work->request.request_id;
        CompletionBackend& backend = *backends_[work->backend_index];
        try {
            std::optional<RequestPayload> payload;
            {
                ConversationReadView conversation = conversation_.read();
                if (!cancellation_.load(std::memory_order_acquire)) {
                    payload = backend.prepare(work->request, conversation);
                }
            }
            if (!payload) {
                publish_terminal(AgentCancelled{request_id});
                continue;
            }
            const CompletionResult result = backend.perform(
                std::move(*payload),
                [this, request_id](CompletionDelta delta) {
                    publish_event(AgentDelta{
                        request_id,
                        delta.kind,
                        std::move(delta.text),
                    });
                },
                cancellation_);
            if (result.outcome == CompletionOutcome::cancelled) {
                publish_terminal(AgentCancelled{request_id});
            } else if (result.outcome == CompletionOutcome::completed) {
                publish_terminal(AgentCompleted{request_id});
            } else {
                publish_terminal(
                    AgentFailed{request_id, result.message});
            }
        } catch (const std::exception& error) {
            publish_terminal(AgentFailed{request_id, error.what()});
        }
    }
}

void AgentRegistry::publish_event(AgentEvent event) {
    if (!events_.push(std::move(event))) {
        throw std::logic_error(
            "Agent event channel closed before execution stopped");
    }
}

void AgentRegistry::publish_terminal(AgentEvent event) {
    request_outstanding_.store(false, std::memory_order_release);
    publish_event(std::move(event));
}

} // namespace cha
