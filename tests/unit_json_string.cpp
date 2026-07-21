#include "json_string.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
namespace {

std::string json_quoted(std::string_view input) {
    std::string output{"\""};
    append_json_string_content(output, input);
    output += '"';
    return output;
}

TEST(JsonString, EscapesControlsAndPreservesUnicode) {
    const std::string input = std::string("quote\" slash\\ control\n\r\t\b\f")
        + '\0' + static_cast<char>(0x1f) + " Â¢ â¬ ð";
    const std::string result = json_quoted(input);

    EXPECT_EQ(result, nlohmann::json(input).dump());
    EXPECT_EQ(nlohmann::json::parse(result).get<std::string>(), input);
}

TEST(JsonString, RejectsMalformedUtf8) {
    for (const std::string& input : {
             std::string("\x80", 1),
             std::string("\xC2", 1),
             std::string("\xC0\x80", 2),
             std::string("\xED\xA0\x80", 3),
             std::string("\xF4\x90\x80\x80", 4),
         }) {
        EXPECT_THROW((void)json_quoted(input), std::runtime_error);
    }
}

TEST(JsonString, HandlesUtf8BoundaryNeighbours) {
    const std::vector<std::string> valid{
        "\xE0\xA0\x80", "\xED\x9F\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF",
    };
    for (const std::string& input : valid) {
        EXPECT_EQ(json_quoted(input), nlohmann::json(input).dump());
    }
    const std::vector<std::string> invalid{
        "\xE0\x9F\x80", "\xED\xA0\x80", "\xF0\x8F\x80\x80", "\xF4\x90\x80\x80",
    };
    for (const std::string& input : invalid) {
        EXPECT_THROW((void)json_quoted(input), std::runtime_error);
    }
}

TEST(JsonString, MatchesNlohmannForFixedRandomCorpus) {
    std::mt19937 generator(0x5a17c0deU);
    std::uniform_int_distribution<int> length_distribution(0, 64);
    std::uniform_int_distribution<int> byte_distribution(0, 255);
    const std::vector<unsigned char> biased_bytes{
        0x00, 0x01, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x1f,
        static_cast<unsigned char>('"'), static_cast<unsigned char>('\\'),
        0x7f, 0x80, 0x81, 0xbf, 0xc2, 0xdf, 0xe0, 0xed, 0xf0, 0xf4, 0xff,
    };
    std::uniform_int_distribution<std::size_t> biased_distribution(0, biased_bytes.size() - 1);
    const std::vector<std::string> valid_utf8{
        "\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF",
        "\xEE\x80\x80", "\xF0\x90\x80\x80", "\xF3\xBF\xBF\xBF", "\xF4\x8F\xBF\xBF",
    };
    std::uniform_int_distribution<std::size_t> utf8_distribution(0, valid_utf8.size() - 1);

    for (int iteration = 0; iteration < 100000; ++iteration) {
        std::string input;
        const int mode = iteration % 4;
        const std::size_t length = static_cast<std::size_t>(length_distribution(generator));
        if (mode == 0) {
            for (std::size_t index = 0; index < length; ++index) {
                input += static_cast<char>(biased_bytes[biased_distribution(generator)]);
            }
        } else if (mode == 1) {
            for (std::size_t index = 0; index < length / 2 + 1; ++index) {
                input += valid_utf8[utf8_distribution(generator)];
            }
        } else {
            input.reserve(length);
            for (std::size_t index = 0; index < length; ++index) {
                input += static_cast<char>(byte_distribution(generator));
            }
        }

        std::string expected;
        try {
            expected = nlohmann::json(input).dump();
        } catch (const nlohmann::json::exception&) {
            EXPECT_THROW((void)json_quoted(input), std::runtime_error)
                << "iteration " << iteration;
            continue;
        }

        try {
            EXPECT_EQ(json_quoted(input), expected) << "iteration " << iteration;
        } catch (const std::exception& error) {
            ADD_FAILURE() << "custom serializer threw at iteration " << iteration
                          << ": " << error.what();
        }
    }
}

} // namespace
} // namespace cha
