#include "agent_registry.h"

#include "completion_client.h"

#include <stdexcept>
#include <utility>

namespace cha {
namespace {

void build_workers(
    std::vector<std::unique_ptr<AgentWorker>>& workers,
    AgentRoster& roster,
    std::unordered_map<std::string, std::size_t>& index,
    const Conversation& conversation,
    AgentEventChannel& events,
    std::vector<std::unique_ptr<CompletionBackend>> backends) {
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
    roster = AgentRoster(std::move(infos));
    for (std::size_t i = 0; i < roster.agents().size(); ++i) {
        index.emplace(roster.agents()[i].id, i);
    }
    workers.reserve(backends.size());
    for (auto& backend : backends) {
        workers.push_back(
            std::make_unique<AgentWorker>(
                conversation, events, std::move(backend)));
    }
}

} // namespace

AgentRegistry::AgentRegistry(
    const Conversation& conversation,
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
    build_workers(
        workers_, roster_, index_, conversation, events_, std::move(backends));
}

AgentRegistry::AgentRegistry(
    const Conversation& conversation,
    std::vector<std::unique_ptr<CompletionBackend>> backends) {
    build_workers(
        workers_, roster_, index_, conversation, events_, std::move(backends));
}

AgentRegistry::~AgentRegistry() noexcept {
    try {
        stop();
    } catch (...) {
    }
}

const AgentRoster& AgentRegistry::roster() const noexcept { return roster_; }

bool AgentRegistry::submit(CompletionRequest request) {
    if (stopped_) {
        return false;
    }
    const auto found = index_.find(request.prompt.addressed_to);
    return found != index_.end()
        && workers_[found->second]->submit(std::move(request));
}

void AgentRegistry::cancel(std::string_view agent_id) {
    if (const auto found = index_.find(std::string(agent_id)); found != index_.end()) {
        workers_[found->second]->cancel();
    }
}

void AgentRegistry::cancel_all() {
    for (const auto& worker : workers_) {
        worker->cancel();
    }
}

ChannelReadStatus AgentRegistry::try_receive(AgentEvent& event) {
    return events_.try_get(event);
}

int AgentRegistry::notification_fd() const { return events_.notification_fd(); }

void AgentRegistry::stop() {
    if (stopped_) {
        return;
    }
    cancel_all();
    for (const auto& worker : workers_) {
        worker->stop();
    }
    events_.close();
    stopped_ = true;
}

} // namespace cha
