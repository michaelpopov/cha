#include "agents/agent_registry.h"

#include "agents/completion_client.h"
#include "util/text.h"

#include <exception>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

std::vector<AgentRuntimeInfo> build_runtime_info(
    const std::vector<std::unique_ptr<CompletionBackend>>& backends) {
    if (backends.empty()) {
        throw std::invalid_argument("Agent registry requires at least one agent");
    }
    std::vector<AgentRuntimeInfo> infos;
    infos.reserve(backends.size());
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const auto& backend : backends) {
        if (!backend) {
            throw std::invalid_argument("Agent registry requires completion backends");
        }
        AgentRuntimeInfo info = backend->info();
        validate_persona_id(info.persona.id);
        validate_persona_name(info.persona.name);
        if (!ids.insert(info.persona.id).second) {
            throw std::invalid_argument(
                "Agent registry has duplicate persona ID '"
                + info.persona.id + "'");
        }
        if (!names.insert(fold_ascii(info.persona.name)).second) {
            throw std::invalid_argument(
                "Agent registry has duplicate persona name '"
                + info.persona.name + "'");
        }
        infos.push_back(std::move(info));
    }
    return infos;
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
    const Transcript& transcript,
    std::vector<AgentDefinition> definitions,
    WakeNotifier& notifier)
    : AgentRegistry(
        transcript,
        build_backends(std::move(definitions)),
        notifier) {
}

AgentRegistry::AgentRegistry(
    const Transcript& transcript,
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    WakeNotifier& notifier)
    : transcript_(transcript),
      backends_(std::move(backends)),
      runtime_info_(build_runtime_info(backends_)),
      notifier_(notifier),
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

const std::vector<AgentRuntimeInfo>& AgentRegistry::runtime_info() const noexcept {
    return runtime_info_;
}

bool AgentRegistry::submit(CompletionRequest request) {
    if (stopped_) {
        return false;
    }

    std::size_t backend_index = runtime_info_.size();
    for (std::size_t index = 0; index < runtime_info_.size(); ++index) {
        if (runtime_info_[index].persona.id == request.prompt.addressed_to) {
            backend_index = index;
            break;
        }
    }
    if (backend_index == runtime_info_.size()) {
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
    notifier_.wake();
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
                TranscriptReadView transcript = transcript_.read();
                if (!cancellation_.load(std::memory_order_acquire)) {
                    payload = backend.prepare(work->request, transcript);
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
            "Agent event queue closed before execution stopped");
    }
    notifier_.wake();
}

void AgentRegistry::publish_terminal(AgentEvent event) {
    request_outstanding_.store(false, std::memory_order_release);
    publish_event(std::move(event));
}

} // namespace cha
