#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace cha {

enum class JsonPurpose {
    character_definition,
    completion_request,
};

template<typename Json>
std::string dump_json(const Json& value, JsonPurpose purpose) {
    try {
        return value.dump();
    } catch (const nlohmann::json::type_error& error) {
        if (error.id == 316) {
            if (purpose == JsonPurpose::character_definition) {
                throw std::runtime_error(
                    "Character definition contains invalid UTF-8");
            }
            throw std::runtime_error(
                "Completion request contains invalid UTF-8");
        }
        throw;
    }
}

} // namespace cha
