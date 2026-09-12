#include "util/path_name.h"
#include "util/text.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace cha {
namespace {

std::string filter_source_references_in_chunks(
    std::string_view input,
    std::size_t chunk_size) {
    std::string pending;
    std::string result;
    for (std::size_t position = 0; position < input.size(); position += chunk_size) {
        pending.append(input.substr(position, chunk_size));
        const std::size_t safe = complete_source_reference_prefix(pending);
        result += remove_source_references(
            std::string_view(pending).substr(0, safe));
        pending.erase(0, safe);
    }
    return result + remove_source_references(pending);
}

TEST(Text, TrimsAllStandardWhitespaceWithoutCopying) {
    const std::string_view input = "\r\n \tvalue \v\f";

    EXPECT_EQ(trim_view(input), "value");
    EXPECT_TRUE(trim_view(" \t\r\n").empty());
}

TEST(Text, FindsTheFirstStandardWhitespaceCharacter) {
    EXPECT_EQ(find_whitespace("command\targument"), 7U);
    EXPECT_EQ(find_whitespace("command\nargument"), 7U);
    EXPECT_EQ(find_whitespace("command"), std::string_view::npos);
}

TEST(Text, FoldsOnlyAsciiLetters) {
    EXPECT_EQ(fold_ascii("Ada_123"), "ada_123");
    EXPECT_EQ(
        fold_ascii("\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD"),
        "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD");
}

TEST(Text, RemovesModelSourceReferences) {
    EXPECT_EQ(
        remove_source_references(
            "Quote ([example.com](https://example.com/source)) done"),
        "Quote  done");
    EXPECT_EQ(
        remove_source_references(
            "Quote ([gutenberg.org](https://www.gutenberg.org/files/3600/"
            "3600-h/3600-h?utm_source=openai)) done"),
        "Quote  done");
    EXPECT_EQ(
        remove_source_references("Keep **([this text))** intact"),
        "Keep **([this text))** intact");
    EXPECT_EQ(
        remove_source_references(
            "Values **([a, b])** matter. Read more "
            "([example.com](https://example.com/source))"),
        "Values **([a, b])** matter. Read more ");
}

TEST(Text, HoldsBackOnlyPlausibleIncompleteSourceReferences) {
    EXPECT_EQ(complete_source_reference_prefix("Intro (["), 6U);
    constexpr std::string_view normal_parenthetical =
        "Intro **([a, b])** continues";
    EXPECT_EQ(
        complete_source_reference_prefix(normal_parenthetical),
        normal_parenthetical.size());

    const std::string long_literal = "Intro ([" + std::string(600, 'x');
    EXPECT_EQ(
        complete_source_reference_prefix(long_literal),
        long_literal.size());
}

TEST(Text, StreamingAndWholeSourceReferenceFilteringAgree) {
    const std::vector<std::string> inputs{
        "Quote ([example.com](https://example.com/source)) done",
        "Values **([a, b])** matter. Read more "
            "([example.com](https://example.com/source))",
        "Quote ([gutenberg.org](https://www.gutenberg.org/files/3600/"
            "3600-h/3600-h?utm_source=openai)) done",
        "Intro ([unterminated",
        "Intro ([" + std::string(600, 'x'),
    };
    for (const std::string& input : inputs) {
        for (std::size_t chunk_size = 1; chunk_size <= 12; ++chunk_size) {
            EXPECT_EQ(
                filter_source_references_in_chunks(input, chunk_size),
                remove_source_references(input));
        }
    }
}

TEST(PathName, AcceptsOneSafePathComponent) {
    EXPECT_NO_THROW(require_path_component("session-1", "sessions"));
}

TEST(PathName, RejectsEmptySpecialAndNestedPaths) {
    const std::filesystem::path source = "characters";

    EXPECT_THROW(require_path_component("", source), std::runtime_error);
    EXPECT_THROW(require_path_component(".", source), std::runtime_error);
    EXPECT_THROW(require_path_component("..", source), std::runtime_error);
    EXPECT_THROW(require_path_component("nested/forum", source), std::runtime_error);
    EXPECT_THROW(require_path_component("nested\\forum", source), std::runtime_error);
    EXPECT_THROW(require_path_component("/absolute", source), std::runtime_error);
    EXPECT_THROW(require_path_component("Q1: Notes", source), std::runtime_error);
    EXPECT_THROW(require_path_component("Report?", source), std::runtime_error);
    EXPECT_THROW(require_path_component("trailing.", source), std::runtime_error);
    EXPECT_THROW(require_path_component("CON", source), std::runtime_error);
    EXPECT_THROW(require_path_component("nul.txt", source), std::runtime_error);
}

TEST(PathName, AcceptsOnlyUrlUnreservedAsciiIdentifiers) {
    EXPECT_TRUE(is_url_safe_identifier("AZaz09-._~"));
    EXPECT_NO_THROW(require_url_safe_identifier("forum-1", "forums"));

    EXPECT_FALSE(is_url_safe_identifier(""));
    EXPECT_FALSE(is_url_safe_identifier("."));
    EXPECT_FALSE(is_url_safe_identifier(".."));
    EXPECT_FALSE(is_url_safe_identifier("has space"));
    EXPECT_FALSE(is_url_safe_identifier("has#fragment"));
    EXPECT_FALSE(is_url_safe_identifier("has?query"));
    EXPECT_FALSE(is_url_safe_identifier("has%escape"));
    EXPECT_FALSE(is_url_safe_identifier("has/slash"));
    EXPECT_FALSE(is_url_safe_identifier("has\\backslash"));
    EXPECT_FALSE(is_url_safe_identifier("na\xc3\xafve"));
    EXPECT_FALSE(is_url_safe_identifier(std::string{"nul\0byte", 8}));
    EXPECT_THROW(
        require_url_safe_identifier("has#fragment", "forums"),
        std::runtime_error);
}

} // namespace
} // namespace cha
