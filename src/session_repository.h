#pragma once

#include <filesystem>
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

// Lists, creates, and resolves persisted sessions for one room without retaining session state.
class SessionRepository {
public:
    SessionRepository(std::filesystem::path directory, std::string room_name, std::string persona_name);

    [[nodiscard]] std::vector<Session> list() const;
    [[nodiscard]] Session create(std::string label) const;
    [[nodiscard]] std::filesystem::path data_path(const std::string& session_id) const;

private:
    std::filesystem::path directory_;
    std::string room_name_;
    std::string persona_name_;
};

} // namespace cha
