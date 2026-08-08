#include "agents/completion_executor.h"

#include "agents/completion_client.h"
#include "util/logging.h"
#include "util/text.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

std::vector<AgentRuntimeInfo> build_runtime_info(
    const std::vector<std::unique_ptr<CompletionBackend>>& backends) {
    if (backends.empty()) {
        throw std::invalid_argument(
            "Completion executor requires at least one agent");
    }
    std::vector<AgentRuntimeInfo> infos;
    infos.reserve(backends.size());
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const auto& backend : backends) {
        if (!backend) {
            throw std::invalid_argument(
                "Completion executor requires completion backends");
        }
        AgentRuntimeInfo info = backend->info();
        validate_character_id(info.character.id);
        validate_character_name_syntax(info.character.name);
        if (!ids.insert(info.character.id).second) {
            throw std::invalid_argument(
                "Completion executor has duplicate character ID '"
                + info.character.id + "'");
        }
        if (!names.insert(fold_ascii(info.character.name)).second) {
            throw std::invalid_argument(
                "Completion executor has duplicate character name '"
                + info.character.name + "'");
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
        const std::string name = definition.config.display_name;
        try {
            backends.push_back(
                std::make_unique<CompletionClient>(std::move(definition)));
        } catch (const std::exception& error) {
            log_error("Character initialization failed: agent_id=" + id
                + " reason=" + error.what());
            throw std::runtime_error(
                "Character '" + name + "' failed to initialize: "
                + error.what());
        }
    }
    return backends;
}

} // namespace

CompletionExecutor::CompletionExecutor(
    std::vector<AgentDefinition> definitions,
    WakeNotifier& notifier,
    ThreadPool& worker_pool)
    : CompletionExecutor(
          build_backends(std::move(definitions)), notifier, worker_pool) {
}

CompletionExecutor::CompletionExecutor(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    WakeNotifier& notifier,
    ThreadPool& worker_pool,
    BeforeSubmitHook before_submit)
    : backends_(std::move(backends)),
      runtime_info_(build_runtime_info(backends_)),
      notifier_(notifier),
      worker_pool_(worker_pool),
      before_submit_(std::move(before_submit)) {
    // Exact equality is deliberate: this pool runs only agent work, and one
    // worker per backend guarantees full-width fan-out.
    if (worker_pool_.worker_count() != backends_.size()) {
        throw std::invalid_argument(
            "Completion executor requires one pool worker per backend");
    }
}

const std::vector<AgentRuntimeInfo>& CompletionExecutor::runtime_info()
    const noexcept {
    return runtime_info_;
}

std::size_t CompletionExecutor::backend_index(std::string_view id) const {
    for (std::size_t index = 0; index < runtime_info_.size(); ++index) {
        if (runtime_info_[index].character.id == id) {
            return index;
        }
    }
    return runtime_info_.size();
}

CompletionBatch CompletionExecutor::stage_batch(
    std::vector<CompletionInput> inputs) {
    if (inputs.empty()) {
        throw std::invalid_argument(
            "Staged batch requires at least one completion input");
    }

    // Resolve every target before any slot is constructed or submitted.
    std::vector<CompletionBackend*> targets;
    targets.reserve(inputs.size());
    std::unordered_set<std::size_t> distinct;
    for (const CompletionInput& input : inputs) {
        if (!input.history) {
            throw std::invalid_argument("Completion input requires history");
        }
        const std::size_t index = backend_index(input.run.target.id);
        if (index == runtime_info_.size()) {
            throw std::invalid_argument("Completion input has unknown target");
        }
        if (!distinct.insert(index).second) {
            throw std::invalid_argument("Staged batch has duplicate targets");
        }
        targets.push_back(backends_[index].get());
    }

    return CompletionBatch::stage(
        std::move(inputs), targets, notifier_, worker_pool_, before_submit_);
}

} // namespace cha
