#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace cha {

// Describes a resolved room, including its selected persona name and storage directory.
struct Room {
    std::string name;
    std::string persona_name;
    std::filesystem::path directory;
};

// Resolves rooms and personas relative to one application workspace root.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root = ".");

    [[nodiscard]] std::vector<std::string> rooms() const;
    [[nodiscard]] Room load_room(const std::string& name) const;
    // Resolves the selected persona directory without loading its agent configuration.
    [[nodiscard]] std::filesystem::path persona_directory(const Room& room) const;

private:
    [[nodiscard]] std::filesystem::path room_directory(const std::string& name) const;
    [[nodiscard]] static std::string read_single_name_list(const std::filesystem::path& path);

    std::filesystem::path root_;
};

} // namespace cha
