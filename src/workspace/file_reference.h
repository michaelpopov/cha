#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cha {

enum class FileReferenceOwner { character, forum };
enum class FileReferenceKind { note, folder };

struct FileReference {
    FileReferenceKind kind;
    std::string ref_id;
    std::string name;

    bool operator==(const FileReference&) const = default;
};

inline std::string_view to_string(FileReferenceKind kind) {
    return kind == FileReferenceKind::note ? "note" : "folder";
}

inline std::optional<FileReferenceKind> parse_file_reference_kind(std::string_view value) {
    if (value == "note") return FileReferenceKind::note;
    if (value == "folder") return FileReferenceKind::folder;
    return std::nullopt;
}

inline std::optional<FileReferenceOwner> parse_file_reference_owner(std::string_view value) {
    if (value == "character") return FileReferenceOwner::character;
    if (value == "forum") return FileReferenceOwner::forum;
    return std::nullopt;
}

} // namespace cha
