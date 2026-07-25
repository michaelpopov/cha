#include "agents/json_serialization.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace cha {
namespace {

template<typename Json>
std::string invalid_utf8_error(JsonPurpose purpose) {
    const Json value = std::string("\xc0\x80", 2);
    try {
        (void)dump_json(value, purpose);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

TEST(JsonSerialization, IdentifiesInvalidAgentDefinitionText) {
    EXPECT_EQ(
        invalid_utf8_error<nlohmann::ordered_json>(
            JsonPurpose::agent_definition),
        "Agent definition contains invalid UTF-8");
}

TEST(JsonSerialization, IdentifiesInvalidCompletionRequestText) {
    EXPECT_EQ(
        invalid_utf8_error<nlohmann::json>(
            JsonPurpose::completion_request),
        "Completion request contains invalid UTF-8");
}

} // namespace
} // namespace cha
