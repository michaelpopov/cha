#include "util/base64.h"

#include <stdexcept>

namespace cha {
namespace {
constexpr std::string_view alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

unsigned value(char ch) {
    const auto index = alphabet.find(ch);
    if (index == std::string_view::npos) {
        throw std::runtime_error("Malformed base64");
    }
    return static_cast<unsigned>(index);
}
} // namespace

std::string encode_base64(std::string_view bytes) {
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned a = static_cast<unsigned char>(bytes[i]);
        const unsigned b = i + 1 < bytes.size()
            ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned c = i + 2 < bytes.size()
            ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        result += alphabet[a >> 2];
        result += alphabet[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return result;
}

std::string decode_base64(std::string_view encoded) {
    if (encoded.size() % 4 != 0) {
        throw std::runtime_error("Malformed base64");
    }
    std::string result;
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const unsigned a = value(encoded[i]);
        const unsigned b = value(encoded[i + 1]);
        const bool pad_c = encoded[i + 2] == '=';
        const bool pad_d = encoded[i + 3] == '=';
        const unsigned c = pad_c ? 0 : value(encoded[i + 2]);
        const unsigned d = pad_d ? 0 : value(encoded[i + 3]);
        if ((pad_c && !pad_d)
            || ((pad_c || pad_d) && i + 4 != encoded.size())
            || (pad_c && (b & 15)) || (pad_d && !pad_c && (c & 3))) {
            throw std::runtime_error("Malformed base64");
        }
        result += static_cast<char>((a << 2) | (b >> 4));
        if (!pad_c) result += static_cast<char>((b << 4) | (c >> 2));
        if (!pad_d) result += static_cast<char>((c << 6) | d);
    }
    return result;
}

} // namespace cha
