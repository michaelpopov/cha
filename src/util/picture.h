#pragma once

#include "util/text.h"

#include <array>
#include <string_view>

namespace cha {

struct PictureFormat {
    std::string_view filename;
    std::string_view mime_type;
};

// Order also defines picture lookup priority.
inline constexpr std::array picture_formats{
    PictureFormat{"PICTURE.png", "image/png"},
    PictureFormat{"PICTURE.webp", "image/webp"},
    PictureFormat{"PICTURE.jpg", "image/jpeg"},
    PictureFormat{"PICTURE.jpeg", "image/jpeg"},
    PictureFormat{"PICTURE.gif", "image/gif"},
};

inline const PictureFormat* picture_format(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    for (const auto& format : picture_formats) {
        if (name == format.filename) return &format;
    }
    return nullptr;
}

// Ignores letter case, so import can warn about names such as PICTURE.PNG.
inline bool supported_picture_extension(std::string_view extension) {
    for (const auto& format : picture_formats) {
        if (ascii_iequals(format.filename.substr(format.filename.find_last_of('.')), extension)) {
            return true;
        }
    }
    return false;
}

} // namespace cha
