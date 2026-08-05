#pragma once

#include "application/application_result.h"

#include <string>

namespace cha {
class ChatApplication;

// Text-layer adapter that combines terminal application commands with the
// existing controller-scoped text dispatcher.
class ApplicationDispatcher {
public:
    explicit ApplicationDispatcher(ChatApplication& application)
        : application_(application) {
    }

    ApplicationResult handle(std::string input);

private:
    ChatApplication& application_;
};
} // namespace cha
