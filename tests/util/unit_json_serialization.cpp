#include "util/json_serialization.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace cha {
namespace {

template<typename Json>
std::string invalid_utf8_error(std::string_view subject) {
    const Json value = std::string("\xc0\x80", 2);
    try {
        (void)dump_json(value, subject);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

TEST(JsonSerialization, IdentifiesInvalidCharacterDefinitionText) {
    EXPECT_EQ(
        invalid_utf8_error<nlohmann::ordered_json>("Character definition"),
        "Character definition contains invalid UTF-8");
}

TEST(JsonSerialization, IdentifiesInvalidModelRequestText) {
    EXPECT_EQ(
        invalid_utf8_error<nlohmann::json>("Model request"),
        "Model request contains invalid UTF-8");
}

} // namespace
} // namespace cha
