#include "agents/agent_registry.h"

#include "agents/completion_client.h"
#include "util/text.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char left_character = left[index];
        char right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(left_character - 'A' + 'a');
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

bool starts_with_folded(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size()
        && ascii_iequals(value.substr(0, prefix.size()), prefix);
}

std::string_view trim_punctuation(std::string_view handle) {
    while (!handle.empty()
           && std::string_view(",.;:!?").find(handle.back())
               != std::string_view::npos) {
        handle.remove_suffix(1);
    }
    return handle;
}

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

AgentRoster::AgentRoster(std::vector<AgentInfo> agents)
    : agents_(std::move(agents)) {
    if (agents_.empty()) {
        throw std::invalid_argument("Agent roster cannot be empty");
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const AgentInfo& agent : agents_) {
        validate_agent_id(agent.id);
        validate_agent_name(agent.name);
        if (!ids.insert(agent.id).second) {
            throw std::invalid_argument(
                "Agent roster has duplicate agent ID '" + agent.id + "'");
        }
        if (!names.insert(fold_ascii(agent.name)).second) {
            throw std::invalid_argument(
                "Agent roster has duplicate agent name '" + agent.name + "'");
        }
    }
}

const std::vector<AgentInfo>& AgentRoster::agents() const noexcept { return agents_; }
const AgentInfo& AgentRoster::first() const { return agents_.front(); }

const AgentInfo* AgentRoster::find(std::string_view id) const {
    const auto found = std::find_if(
        agents_.begin(), agents_.end(),
        [id](const AgentInfo& agent) { return agent.id == id; });
    return found == agents_.end() ? nullptr : &*found;
}

HandleResolution AgentRoster::resolve_handle(std::string_view handle) const {
    if (handle.empty()) {
        return {};
    }
    const auto named = [this](std::string_view value) -> const AgentInfo* {
        const auto found = std::find_if(agents_.begin(), agents_.end(), [value](const AgentInfo& agent) {
            return ascii_iequals(agent.name, value);
        });
        return found == agents_.end() ? nullptr : &*found;
    };
    if (const AgentInfo* agent = named(handle)) {
        return {HandleMatch::resolved, agent, {}};
    }
    const std::string_view trimmed = trim_punctuation(handle);
    if (trimmed != handle) {
        if (const AgentInfo* agent = named(trimmed)) {
            return {HandleMatch::resolved, agent, {}};
        }
    }
    if (trimmed.empty()) {
        return {};
    }
    std::vector<const AgentInfo*> candidates;
    for (const AgentInfo& agent : agents_) {
        if (starts_with_folded(agent.name, trimmed)) {
            candidates.push_back(&agent);
        }
    }
    if (candidates.size() == 1) {
        return {HandleMatch::resolved, candidates.front(), {}};
    }
    if (candidates.empty()) {
        return {};
    }
    return {HandleMatch::ambiguous, nullptr, std::move(candidates)};
}

std::string AgentRoster::handle_list() const {
    std::string result;
    for (const AgentInfo& agent : agents_) {
        if (!result.empty()) result += ", ";
        result += "@" + agent.name;
    }
    return result;
}

AgentRegistry::AgentRegistry(
    const Transcript& transcript,
    std::vector<AgentDefinition> definitions)
    : AgentRegistry(transcript, build_backends(std::move(definitions))) {
}

AgentRegistry::AgentRegistry(
    const Transcript& transcript,
    std::vector<std::unique_ptr<CompletionBackend>> backends)
    : transcript_(transcript),
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
            "Agent event channel closed before execution stopped");
    }
}

void AgentRegistry::publish_terminal(AgentEvent event) {
    request_outstanding_.store(false, std::memory_order_release);
    publish_event(std::move(event));
}

} // namespace cha
