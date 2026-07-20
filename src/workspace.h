#pragma once

#include "config.h"

#include <filesystem>
#include <string>
#include <vector>

namespace cha {

struct Room {
    std::string name;
    std::string persona_name;
    Config config;
    std::filesystem::path directory;
};

struct Session {
    std::string id;
    std::string label;

    bool operator==(const Session&) const = default;
};

class Workspace {
public:
    explicit Workspace(std::filesystem::path root = ".");

    [[nodiscard]] std::vector<std::string> rooms() const;
    [[nodiscard]] Room load_room(const std::string& name) const;
    [[nodiscard]] std::vector<Session> sessions(const Room& room) const;
    [[nodiscard]] Session create_session(const Room& room, std::string label) const;
    [[nodiscard]] std::filesystem::path session_data_path(const Room& room, const std::string& session) const;

private:
    [[nodiscard]] std::filesystem::path room_directory(const std::string& name) const;
    [[nodiscard]] static std::string read_single_name_list(const std::filesystem::path& path);

    std::filesystem::path root_;
};

} // namespace cha
