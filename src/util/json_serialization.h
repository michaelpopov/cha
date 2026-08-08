#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace cha {

template<typename Json>
std::string dump_json(const Json& value, std::string_view invalid_utf8_subject) {
    try {
        return value.dump();
    } catch (const nlohmann::json::type_error& error) {
        if (error.id == 316) {
            throw std::runtime_error(
                std::string(invalid_utf8_subject) + " contains invalid UTF-8");
        }
        throw;
    }
}

} // namespace cha
