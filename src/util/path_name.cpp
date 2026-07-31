#include "util/path_name.h"

#include "util/utf8_path.h"

#include <stdexcept>
#include <string>

namespace cha {

void require_path_component(std::string_view name, const std::filesystem::path& source) {
    const std::filesystem::path path = path_from_utf8(name);
    if (name.empty()
        || name.find('/') != std::string_view::npos
        || name.find('\\') != std::string_view::npos
        || path.is_absolute()
        || path.has_parent_path()
        || name == "."
        || name == "..") {
        throw std::runtime_error(
            "Invalid name '" + std::string(name) + "' in '"
            + utf8_path(source) + "'");
    }
}

} // namespace cha
