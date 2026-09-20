#pragma once

#include "util/path_name.h"

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace cha::bridge {

inline std::string require_identifier(
    const nlohmann::json& params,
    std::string_view key) {
    if (!params.contains(std::string(key)) || !params[std::string(key)].is_string()) {
        throw std::invalid_argument("The request was not valid.");
    }
    const std::string& value =
        params[std::string(key)].get_ref<const std::string&>();
    if (!is_url_safe_identifier(value)) {
        throw std::invalid_argument("The request was not valid.");
    }
    return value;
}

inline void require_only_keys(
    const nlohmann::json& params,
    std::initializer_list<std::string_view> keys) {
    // Zero-param methods still require an empty object, not omitted keys.
    if (!params.is_object() || params.size() != keys.size()) {
        throw std::invalid_argument("The request was not valid.");
    }
    for (std::string_view key : keys) {
        if (!params.contains(std::string(key))) {
            throw std::invalid_argument("The request was not valid.");
        }
    }
}

inline std::string require_string(const nlohmann::json& params, std::string_view key) {
    const std::string name(key);
    if (!params.contains(name) || !params[name].is_string()) {
        throw std::invalid_argument("The request was not valid.");
    }
    return params[name].get<std::string>();
}

inline nlohmann::json without_key(nlohmann::json params, std::string_view key) {
    params.erase(std::string(key));
    return params;
}

} // namespace cha::bridge
