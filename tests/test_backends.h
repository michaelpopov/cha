#pragma once

#include "completion_backend.h"

#include <memory>
#include <utility>
#include <vector>

namespace cha::test {

inline std::vector<std::unique_ptr<CompletionBackend>> one_backend(
    std::unique_ptr<CompletionBackend> backend) {
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    return backends;
}

} // namespace cha::test
