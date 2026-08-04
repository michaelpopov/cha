#include "util/public_name.h"
#include "util/text.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>

namespace cha {
namespace {

const std::filesystem::path source{"public.toml"};

TEST(PublicName, AcceptsInternalWhitespaceAndPreservesAuthoredCasing) {
    EXPECT_NO_THROW(validate_public_name("The Lobby", "Forum name", source));
    EXPECT_EQ(fold_ascii("The Lobby"), "the lobby");
    EXPECT_EQ(fold_ascii("READER"), fold_ascii("Reader"));
}

TEST(PublicName, RejectsInvalidTextAndParticipantPrefixes) {
    for (const std::string value : {"", " Reader", "Reader ", "Reader\nName", "Reader\x01", "\xC0\x80"}) {
        EXPECT_THROW(validate_public_name(value, "Persona name", source, true), std::runtime_error);
    }
    EXPECT_THROW(validate_public_name("@Reader", "Persona name", source, true), std::runtime_error);
    EXPECT_THROW(validate_public_name("/Reader", "Persona name", source, true), std::runtime_error);
    EXPECT_NO_THROW(validate_public_name("/Forum", "Forum name", source));
}

TEST(PublicName, DescriptionsAreSingleNonemptyTrimmedLines) {
    EXPECT_NO_THROW(validate_description("Short summary", "Persona", source));
    for (const std::string value : {"", " summary", "summary ", "summary\nmore"}) {
        EXPECT_THROW(validate_description(value, "Persona", source), std::runtime_error);
    }
}

} // namespace
} // namespace cha
