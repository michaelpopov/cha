#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// Describes a resolved room, including its ordered persona roster and storage directory.
struct Room {
    std::string name;
    std::vector<std::string> persona_names;
    std::filesystem::path directory;
};

// Resolves rooms and personas relative to one application workspace root.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root = ".");

    [[nodiscard]] std::vector<std::string> rooms() const;
    [[nodiscard]] Room load_room(const std::string& name) const;
    // Resolves the selected persona directory without loading its agent configuration.
    [[nodiscard]] std::filesystem::path persona_directory(std::string_view persona_name) const;

private:
    [[nodiscard]] std::filesystem::path room_directory(const std::string& name) const;
    [[nodiscard]] static std::vector<std::string> read_name_list(const std::filesystem::path& path);

    std::filesystem::path root_;
};

} // namespace cha
