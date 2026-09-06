#pragma once

#include "web/application_config.h"

#include <mutex>
#include <utility>

namespace cha::web {

// Holds the active vault definition. The mutex protects this value only; it
// is not an atomic switch of runtime resources.
class CurrentVault {
public:
    explicit CurrentVault(VaultDefinition vault) : vault_(std::move(vault)) {}

    [[nodiscard]] VaultDefinition get() const {
        const std::lock_guard lock(mutex_);
        return vault_;
    }

    void set(VaultDefinition vault) {
        const std::lock_guard lock(mutex_);
        vault_ = std::move(vault);
    }

private:
    mutable std::mutex mutex_;
    VaultDefinition vault_;
};

} // namespace cha::web
