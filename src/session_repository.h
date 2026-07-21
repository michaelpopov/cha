#pragma once

#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cha {

// Identifies a persisted session by its ID, display label, and optional validation error.
struct Session {
    std::string id;
    std::string label;
    std::string error;

    bool operator==(const Session&) const = default;
};

// Lists, creates, and resolves self-contained SQLite sessions for one room.
class SessionRepository {
public:
    using Clock = std::function<std::time_t()>;

    SessionRepository(
        std::filesystem::path directory,
        std::string room_name,
        Clock clock = {});

    [[nodiscard]] std::vector<Session> list() const;
    [[nodiscard]] Session create(std::string label) const;
    [[nodiscard]] std::filesystem::path database_path(const std::string& session_id) const;
    // Revalidates the embedded identity before returning the selected database.
    [[nodiscard]] std::filesystem::path open_database_path(const std::string& session_id) const;

private:
    std::filesystem::path directory_;
    std::string room_name_;
    Clock clock_;
};

} // namespace cha
