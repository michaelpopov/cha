#include "agent_registry.h"

#include "completion_client.h"

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
    : roster_(build_roster(backends)) {
    workers_.reserve(backends.size());
    for (auto& backend : backends) {
        workers_.push_back(
            std::make_unique<AgentWorker>(
                conversation, events_, std::move(backend)));
    }
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
    const std::vector<AgentInfo>& agents = roster_.agents();
    for (std::size_t index = 0; index < agents.size(); ++index) {
        if (agents[index].id == request.prompt.addressed_to) {
            return workers_[index]->submit(std::move(request));
        }
    }
    return false;
}

void AgentRegistry::cancel(std::string_view agent_id) {
    const std::vector<AgentInfo>& agents = roster_.agents();
    for (std::size_t index = 0; index < agents.size(); ++index) {
        if (agents[index].id == agent_id) {
            workers_[index]->cancel();
            return;
        }
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
