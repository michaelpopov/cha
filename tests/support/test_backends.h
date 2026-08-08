#pragma once

#include "agents/model_backend.h"

#include <memory>
#include <utility>
#include <vector>

namespace cha::test {

inline std::vector<std::unique_ptr<ModelBackend>> one_backend(
    std::unique_ptr<ModelBackend> backend) {
    std::vector<std::unique_ptr<ModelBackend>> backends;
    backends.push_back(std::move(backend));
    return backends;
}

} // namespace cha::test
