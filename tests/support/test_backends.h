#pragma once

#include "agents/character.h"
#include "providers/model_backend.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cha::test {

struct DescribedBackendInfo {
    CharacterMetadata character;
    std::string model;
    std::string api;
    bool streaming{};
};

// A shareable test scenario: it carries character metadata and coordinates
// observations, while the controller harness creates the actual request-local
// ModelBackend facade.
class DescribedModelBackend {
public:
    virtual ~DescribedModelBackend() = default;

    virtual RequestPayload prepare(const GenerationRequest& input) = 0;
    virtual GenerationResult perform(
        RequestPayload payload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) = 0;
    virtual DescribedBackendInfo info() const = 0;
};

inline std::vector<std::unique_ptr<DescribedModelBackend>> one_backend(
    std::unique_ptr<DescribedModelBackend> backend) {
    std::vector<std::unique_ptr<DescribedModelBackend>> backends;
    backends.push_back(std::move(backend));
    return backends;
}

} // namespace cha::test
