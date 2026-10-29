#pragma once

#include "util/text.h"

#include <array>
#include <string_view>

namespace cha {

struct PictureFormat {
    std::string_view extension;
    std::string_view mime_type;
};

// Order also defines picture lookup priority.
inline constexpr std::array picture_formats{
    PictureFormat{".png", "image/png"},
    PictureFormat{".webp", "image/webp"},
    PictureFormat{".jpg", "image/jpeg"},
    PictureFormat{".jpeg", "image/jpeg"},
    PictureFormat{".gif", "image/gif"},
};

inline const PictureFormat* picture_format(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const auto name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos) return nullptr;
    for (const auto& format : picture_formats) {
        if (ascii_iequals(name.substr(dot), format.extension)) {
            return &format;
        }
    }
    return nullptr;
}

// Ignores letter case.
inline bool supported_picture_extension(std::string_view extension) {
    for (const auto& format : picture_formats) {
        if (ascii_iequals(format.extension, extension)) {
            return true;
        }
    }
    return false;
}

} // namespace cha
