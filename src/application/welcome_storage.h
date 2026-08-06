#pragma once

#include "session/session_catalog.h"

#include <filesystem>

namespace cha {

class WelcomeStorage {
public:
    WelcomeStorage();
    ~WelcomeStorage();
    WelcomeStorage(const WelcomeStorage&) = delete;
    WelcomeStorage& operator=(const WelcomeStorage&) = delete;
    const std::filesystem::path& directory() const noexcept { return directory_; }
    const std::filesystem::path& database_path() const noexcept { return database_path_; }
    std::filesystem::file_time_type last_write_time() const;
    PreparedSession prepare();

private:
    std::filesystem::path directory_;
    std::filesystem::path database_path_;
};
} // namespace cha
