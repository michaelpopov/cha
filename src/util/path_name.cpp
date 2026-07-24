#include "util/path_name.h"

#include <stdexcept>
#include <string>

namespace cha {

void require_path_component(std::string_view name, const std::filesystem::path& source) {
    const std::filesystem::path path{name};
    if (name.empty() || path.is_absolute() || path.has_parent_path() || name == "." || name == "..") {
        throw std::runtime_error("Invalid name '" + std::string(name) + "' in '" + source.string() + "'");
    }
}

} // namespace cha
