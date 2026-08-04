#include "application/welcome_storage.h"

#include "application/builtins.h"
#include "session/session_database.h"
#include "session/session_lease.h"
#include "util/logging.h"

#include <chrono>
#include <filesystem>
#include <random>
#include <stdexcept>

namespace cha {
WelcomeStorage::WelcomeStorage() {
    const std::filesystem::path parent = std::filesystem::temp_directory_path();
    std::mt19937_64 random(std::random_device{}());
    for (std::size_t attempt{}; attempt != 100; ++attempt) {
        const auto candidate = parent / ("cha-welcome-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(random()));
        std::error_code error;
        if (std::filesystem::create_directory(candidate, error)) {
            directory_ = candidate;
            database_path_ = directory_ / "welcome.sqlite3";
            try {
                std::filesystem::permissions(
                    directory_, std::filesystem::perms::owner_all,
                    std::filesystem::perm_options::replace);
                if (!create_session_database(database_path_, {.id = "builtin-welcome", .forum = "builtin-entrance", .label = std::string(welcome_name)})) {
                    throw std::runtime_error("Failed to create Welcome session database");
                }
                return;
            } catch (...) {
                std::error_code cleanup_error;
                std::filesystem::remove_all(directory_, cleanup_error);
                directory_.clear();
                database_path_.clear();
                throw;
            }
        }
    }
    throw std::runtime_error("Failed to create private Welcome storage");
}

WelcomeStorage::~WelcomeStorage() {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    if (error) log_warn("Failed to clean up Welcome storage");
}

PreparedSession WelcomeStorage::prepare() {
    SessionLease lease = SessionLease::acquire(database_path_);
    return {{"builtin-welcome", std::string(welcome_name)}, database_path_, std::move(lease)};
}
} // namespace cha
