#include "util/path_name.h"
#include "util/text.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

namespace cha {
namespace {

std::string filter_url_references_in_chunks(
    std::string_view input,
    std::size_t chunk_size) {
    UrlReferenceFilter filter;
    std::string result;
    for (std::size_t position = 0; position < input.size(); position += chunk_size) {
        result += filter.push(input.substr(position, chunk_size));
    }
    return result + filter.finish();
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

struct UrlCase {
    std::string input;
    std::string expected;
};

const std::vector<UrlCase>& url_cases() {
    static const std::vector<UrlCase> cases{
        {"Quote ([example.com](https://example.com/source)) done", "Quote done"},
        {"Quote ([gutenberg.org](https://www.gutenberg.org/files/3600/"
            "3600-h/3600-h?utm_source=openai)) done", "Quote done"},
        {"Keep **([this text))** intact", "Keep **([this text))** intact"},
        {"Values **([a, b])** matter. Read more "
            "([example.com](https://example.com/source))",
            "Values **([a, b])** matter. Read more"},
        {"Both ([cnn.com](https://cnn.com/a), [bbc.com](https://bbc.com/b)) agree.",
            "Both agree."},
        // A descriptive label is text; a label that only names the site is a citation.
        {"Read [the full report](https://example.com/report) today.",
            "Read the full report today."},
        {"Rates rose [cnn.com](https://edition.cnn.com/a).", "Rates rose."},
        {"Rates fell [1](https://example.com/1).", "Rates fell."},
        {"See ![chart](https://example.com/chart.png) below", "See below"},
        {"Wiki (https://en.wikipedia.org/wiki/Foo_(bar)) says", "Wiki says"},
        {"Visit <https://example.com> now", "Visit now"},
        {"Try **www.example.net** and `https://example.org` now", "Try and now"},
        {"Source https://example.com/a?b=1.", "Source."},
        {"Keep [a note](#part), [x], and f(y)", "Keep [a note](#part), [x], and f(y)"},
        // Lines that only held links disappear; other lines keep their text.
        {"Intro\n- [cnn.com](https://cnn.com/a)\n  * https://bbc.com/b\n"
            "[1]: https://example.com/1\n1. <https://example.com/2>\n"
            "> [bbc.com](https://www.bbc.com/news) \n"
            "https://example.com/3 is here\n**Source:** https://example.com/4\nEnd",
            "Intro\nis here\n**Source:**\nEnd"},
        {"Mixed\r\n- https://example.com/a\r\n12 apples https://example.com/b\r\nDone",
            "Mixed\r\n12 apples\r\nDone"},
        {"Intro ([unterminated", "Intro ([unterminated"},
        {"Intro ([" + std::string(600, 'x'), "Intro ([" + std::string(600, 'x')},
    };
    return cases;
}

TEST(Text, RemovesUrlReferences) {
    for (const UrlCase& test : url_cases()) {
        EXPECT_EQ(remove_url_references(test.input), test.expected) << test.input;
    }
}

TEST(Text, StreamingUrlFilterReleasesPlainTextAndHoldsPossibleLinks) {
    UrlReferenceFilter filter;
    EXPECT_EQ(filter.push("Hello wor"), "Hello wor");
    // A last word that can still grow into a link waits for more text.
    EXPECT_EQ(filter.push("ld. See h"), "ld. See");
    // A possible link holds the rest of its line until the line ends.
    EXPECT_EQ(filter.push("ttps://example.com/a and more"), "");
    EXPECT_EQ(filter.push(" text\nNext line"), " and more text\nNext line");
    // A list marker waits for its line, so a line of links disappears whole.
    EXPECT_EQ(filter.push("\n- "), "\n");
    EXPECT_EQ(filter.push("[example.com](https://example.com)\n"), "");
    EXPECT_EQ(filter.push("(see ab"), "");
    EXPECT_EQ(filter.finish(), "(see ab");
}

TEST(Text, StreamingAndWholeUrlFilteringAgree) {
    std::vector<std::string> inputs;
    for (const UrlCase& test : url_cases()) inputs.push_back(test.input);
    inputs.insert(inputs.end(), {
        "* **https://example.com/a**\n- text https://example.com/c\n",
        "abc" "www.example.com and !" "[alt](https://example.com/a.png) done",
        "1. https://example.com/a\n-- https://example.com/b\nEnd",
        // Cases that once differed: digits that join a later "." or ")" into
        // a list marker, and a mark that did not directly wrap the URL.
        "12  www.\n12 https://example.com/a)\nEnd",
        "word* https://example.com/a <https://example.com/b>*",
        "\" <https://example.com/a>www.example.com\"x",
    });
    for (const std::string& input : inputs) {
        for (std::size_t chunk_size = 1; chunk_size <= 12; ++chunk_size) {
            EXPECT_EQ(
                filter_url_references_in_chunks(input, chunk_size),
                remove_url_references(input))
                << input << " in chunks of " << chunk_size;
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
