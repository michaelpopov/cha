#include "agents/generation_executor.h"

#include "agents/provider_client.h"
#include "util/logging.h"
#include "util/text.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

std::vector<ModelBackendInfo> build_runtime_info(
    const std::vector<std::unique_ptr<ModelBackend>>& backends) {
    if (backends.empty()) {
        throw std::invalid_argument(
            "Generation executor requires at least one character");
    }
    std::vector<ModelBackendInfo> infos;
    infos.reserve(backends.size());
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const auto& backend : backends) {
        if (!backend) {
            throw std::invalid_argument(
                "Generation executor requires model backends");
        }
        ModelBackendInfo info = backend->info();
        validate_character_id(info.character.id);
        validate_character_display_name_syntax(info.character.display_name);
        if (!ids.insert(info.character.id).second) {
            throw std::invalid_argument(
                "Generation executor has duplicate character ID '"
                + info.character.id + "'");
        }
        if (!names.insert(fold_ascii(info.character.display_name)).second) {
            throw std::invalid_argument(
                "Generation executor has duplicate character name '"
                + info.character.display_name + "'");
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

std::vector<ModelBackendInfo> build_runtime_info(
    const std::vector<SharedCharacterDefinition>& definitions) {
    if (definitions.empty()) {
        throw std::invalid_argument(
            "Generation executor requires at least one character");
    }
    std::vector<ModelBackendInfo> infos;
    infos.reserve(definitions.size());
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const SharedCharacterDefinition& definition : definitions) {
        if (!definition) {
            throw std::invalid_argument(
                "Generation executor requires character definitions");
        }
        ModelBackendInfo info = model_backend_info(*definition);
        validate_character_id(info.character.id);
        validate_character_display_name_syntax(info.character.display_name);
        if (!ids.insert(info.character.id).second) {
            throw std::invalid_argument(
                "Generation executor has duplicate character ID '"
                + info.character.id + "'");
        }
        if (!names.insert(fold_ascii(info.character.display_name)).second) {
            throw std::invalid_argument(
                "Generation executor has duplicate character name '"
                + info.character.display_name + "'");
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

std::unique_ptr<ModelBackend> default_backend_factory(
    SharedCharacterDefinition definition) {
    if (!definition) {
        throw std::invalid_argument("Provider client requires a character definition");
    }
    const std::string id = definition->character.id;
    const std::string display_name = definition->character.display_name;
    try {
        return std::make_unique<ProviderClient>(std::move(definition));
    } catch (const std::exception& error) {
        log_error("Model backend initialization failed: character_id=" + id
            + " reason=" + error.what());
        throw std::runtime_error(
            "Character '" + display_name + "' failed to initialize: "
            + error.what());
    }
}

std::vector<std::unique_ptr<ModelBackend>> build_backends(
    const std::vector<SharedCharacterDefinition>& definitions,
    const ProviderClientFactory& client_factory) {
    std::vector<std::unique_ptr<ModelBackend>> backends;
    backends.reserve(definitions.size());
    for (const SharedCharacterDefinition& definition : definitions) {
        if (!definition) {
            throw std::invalid_argument(
                "Generation executor requires character definitions");
        }
    }
    for (const SharedCharacterDefinition& definition : definitions) {
        std::unique_ptr<ModelBackend> backend = client_factory
            ? client_factory(definition)
            : default_backend_factory(definition);
        if (!backend) {
            throw std::invalid_argument(
                "Generation executor factory returned a null model backend");
        }
        backends.push_back(std::move(backend));
    }
    return backends;
}

} // namespace

GenerationExecutor::GenerationExecutor(
    std::vector<SharedCharacterDefinition> definitions,
    WakeNotifier& notifier,
    ThreadPool& worker_pool,
    ProviderClientFactory client_factory)
    : backends_(build_backends(definitions, client_factory)),
      runtime_info_(build_runtime_info(definitions)),
      notifier_(notifier),
      worker_pool_(worker_pool) {
    if (worker_pool_.worker_count() != backends_.size()) {
        throw std::invalid_argument(
            "Generation executor requires one pool worker per backend");
    }
}

GenerationExecutor::GenerationExecutor(
    std::vector<std::unique_ptr<ModelBackend>> backends,
    WakeNotifier& notifier,
    ThreadPool& worker_pool,
    BeforeSubmitHook before_submit)
    : backends_(std::move(backends)),
      runtime_info_(build_runtime_info(backends_)),
      notifier_(notifier),
      worker_pool_(worker_pool),
      before_submit_(std::move(before_submit)) {
    // Exact equality is deliberate: this pool runs only generation work, and one
    // worker per backend guarantees full-width fan-out.
    if (worker_pool_.worker_count() != backends_.size()) {
        throw std::invalid_argument(
            "Generation executor requires one pool worker per backend");
    }
}

const std::vector<ModelBackendInfo>& GenerationExecutor::runtime_info()
    const noexcept {
    return runtime_info_;
}

std::size_t GenerationExecutor::backend_index(std::string_view id) const {
    for (std::size_t index = 0; index < runtime_info_.size(); ++index) {
        if (runtime_info_[index].character.id == id) {
            return index;
        }
    }
    return runtime_info_.size();
}

GenerationBatch GenerationExecutor::stage_batch(
    std::vector<GenerationRequest> inputs) {
    if (inputs.empty()) {
        throw std::invalid_argument(
            "Staged batch requires at least one generation request");
    }

    // Resolve every target before any slot is constructed or submitted.
    std::vector<ModelBackend*> targets;
    targets.reserve(inputs.size());
    std::unordered_set<std::size_t> distinct;
    for (const GenerationRequest& input : inputs) {
        if (!input.history) {
            throw std::invalid_argument("Generation request requires history");
        }
        const std::size_t index = backend_index(input.run.target.id);
        if (index == runtime_info_.size()) {
            throw std::invalid_argument("Generation request has unknown target");
        }
        if (!distinct.insert(index).second) {
            throw std::invalid_argument("Staged batch has duplicate targets");
        }
        targets.push_back(backends_[index].get());
    }

    return GenerationBatch::stage(
        std::move(inputs), targets, notifier_, worker_pool_, before_submit_);
}

} // namespace cha
