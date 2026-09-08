#pragma once

#include "web/application_config.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cha::web {

// Holds the active vault definition and the names exposed with it. This is not
// an atomic switch of the runtime resources that belong to the active vault.
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

    void set(VaultDefinition vault, std::vector<std::string> names) {
        const std::lock_guard lock(mutex_);
        vault_ = std::move(vault);
        names_ = std::move(names);
    }

    [[nodiscard]] std::pair<VaultDefinition, std::vector<std::string>>
    snapshot() const {
        const std::lock_guard lock(mutex_);
        return {vault_, names_};
    }

    void set_names(std::vector<std::string> names) {
        const std::lock_guard lock(mutex_);
        names_ = std::move(names);
    }

private:
    mutable std::mutex mutex_;
    VaultDefinition vault_;
    std::vector<std::string> names_;
};

} // namespace cha::web
