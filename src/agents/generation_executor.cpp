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

std::unique_ptr<ModelBackend> default_backend_factory(
    CharacterDefinition definition) {
    const std::string id = definition.character.id;
    const std::string display_name = definition.character.display_name;
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
    std::vector<CharacterDefinition> definitions,
    const GenerationExecutor::BackendFactory& backend_factory) {
    std::vector<std::unique_ptr<ModelBackend>> backends;
    backends.reserve(definitions.size());
    for (CharacterDefinition& definition : definitions) {
        backends.push_back(
            backend_factory
                ? backend_factory(std::move(definition))
                : default_backend_factory(std::move(definition)));
    }
    return backends;
}

} // namespace

GenerationExecutor::GenerationExecutor(
    std::vector<CharacterDefinition> definitions,
    WakeNotifier& notifier,
    ThreadPool& worker_pool,
    BackendFactory backend_factory)
    // The delegated constructor consumes the backends, so the definitions are
    // copied into build_backends here and kept as rebuild recipes below.
    : GenerationExecutor(
          build_backends(definitions, backend_factory), notifier, worker_pool) {
    rebuild_recipes_ = std::move(definitions);
    backend_factory_ = backend_factory
        ? std::move(backend_factory)
        : BackendFactory(default_backend_factory);
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

std::size_t GenerationExecutor::recipe_index(const CharacterId& character_id) const {
    if (rebuild_recipes_.empty()) {
        throw std::logic_error(
            "Generation executor has no definitions to rebuild a backend from");
    }
    const std::size_t index = backend_index(character_id);
    if (index == runtime_info_.size()) {
        throw std::invalid_argument(
            "Backend rebuild has unknown target '" + character_id + "'");
    }
    return index;
}

void GenerationExecutor::replace_backend(
    CharacterId character_id,
    const ModelBackendConfig& config) {
    // rebuild_recipes_ and runtime_info_ were built from the same definitions
    // in the same order, so the recipe sits at the slot's index.
    const std::size_t index = recipe_index(character_id);
    CharacterDefinition definition = rebuild_recipes_[index];
    definition.backend = config;
    // Construct before swapping: a factory throw leaves the old slot in place.
    std::unique_ptr<ModelBackend> replacement =
        backend_factory_(std::move(definition));
    if (!replacement) {
        throw std::runtime_error(
            "Backend factory returned no backend for character '"
            + character_id + "'");
    }
    ModelBackendInfo info = replacement->info();
    if (info.character.id != character_id) {
        throw std::runtime_error(
            "Backend factory returned a backend for character '"
            + info.character.id + "' instead of '" + character_id + "'");
    }
    backends_[index] = std::move(replacement);
    runtime_info_[index] = std::move(info);
}

void GenerationExecutor::reset_backend(CharacterId character_id) {
    // The recipe's own configuration, copied because replace_backend rebuilds
    // from that same recipe.
    const ModelBackendConfig config =
        rebuild_recipes_[recipe_index(character_id)].backend;
    replace_backend(std::move(character_id), config);
}

} // namespace cha
