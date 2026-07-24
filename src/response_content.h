#pragma once

#include <string>

namespace cha {

enum class CompletionDeltaKind {
    reasoning,
    answer,
};

struct CompletionDelta {
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

} // namespace cha
