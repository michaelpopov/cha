#pragma once

#include "agents/character.h"
#include "agents/model_backend.h"
#include "agents/generation_batch.h"
#include "util/thread_pool.h"
#include "util/wake_notifier.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace cha {

// The session-lived supplier of model backends. It owns one backend per
// forum character plus their public runtime metadata, and stages new operations
// into the borrowed worker pool. A slot can be rebuilt while idle — a runtime
// provider override replaces one character's backend without touching the
// others.
//
// It deliberately keeps no current batch: staging hands complete ownership of
// one operation to the caller, so there is nothing here addressed to an
// implicit batch.
class GenerationExecutor final {
public:
    // Test-only fault injection immediately before each pool submission. The
    // argument is the zero-based submission index and exceptions abort staging.
    using BeforeSubmitHook = std::function<void(std::size_t)>;

    // Builds one backend from one character definition. The default constructs
    // a ProviderClient; tests substitute fakes. The definition is moved in and
    // carries the backend configuration the backend must run.
    using BackendFactory =
        std::function<std::unique_ptr<ModelBackend>(CharacterDefinition)>;

    // The caller owns the pool and must join it while this executor and its
    // notifier remain alive.
    GenerationExecutor(
        std::vector<CharacterDefinition> definitions,
        WakeNotifier& notifier,
        ThreadPool& worker_pool,
        BackendFactory backend_factory = {});
    GenerationExecutor(
        std::vector<std::unique_ptr<ModelBackend>> backends,
        WakeNotifier& notifier,
        ThreadPool& worker_pool,
        BeforeSubmitHook before_submit = {});

    GenerationExecutor(const GenerationExecutor&) = delete;
    GenerationExecutor& operator=(const GenerationExecutor&) = delete;

    [[nodiscard]] const std::vector<ModelBackendInfo>& runtime_info()
        const noexcept;

    // Owner-thread staging. Strong guarantee: on success every task was
    // accepted by the pool but is held behind the returned batch's unopened
    // gate. On failure no backend is called and no task remains live.
    [[nodiscard]] GenerationBatch stage_batch(
        std::vector<GenerationRequest> inputs);

    // Owner-thread backend replacement, for a runtime provider override. Both
    // rebuild the character's slot from its retained definition —
    // replace_backend with a new backend configuration, reset_backend with
    // the definition's own. Call only while no batch borrows the backends.
    // A factory throw leaves the existing slot in place. Both throw for an
    // unknown character, a null factory result, or a backend reporting a
    // different character ID.
    void replace_backend(CharacterId character_id, const ModelBackendConfig& config);
    void reset_backend(CharacterId character_id);

private:
    [[nodiscard]] std::size_t backend_index(std::string_view id) const;
    // The slot a rebuild addresses. Throws for an unknown character, or when
    // this executor was built from ready-made backends and holds no recipes.
    [[nodiscard]] std::size_t recipe_index(const CharacterId& character_id) const;

    std::vector<std::unique_ptr<ModelBackend>> backends_;
    std::vector<ModelBackendInfo> runtime_info_;
    // The definitions the backends were built from, kept as rebuild recipes
    // for replace_backend/reset_backend. Empty when constructed from
    // ready-made backends, in which case replacement is unavailable.
    std::vector<CharacterDefinition> rebuild_recipes_;
    BackendFactory backend_factory_;
    WakeNotifier& notifier_;
    ThreadPool& worker_pool_;
    BeforeSubmitHook before_submit_;
};

} // namespace cha
