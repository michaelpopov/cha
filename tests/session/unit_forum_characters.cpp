#include "session/forum_characters.h"

#include <gtest/gtest.h>

namespace cha {
namespace {
CharacterMetadata character(std::string id, std::string name) {
    return {std::move(id), std::move(name)};
}
}

TEST(ForumCharacters, ResolvesNamesPunctuationAndUniquePrefixes) {
    ForumCharacters characters({character("ada", "Ada"), character("grace", "Grace")});
    EXPECT_EQ(characters.resolve_handle("ADA").character->id, "ada");
    EXPECT_EQ(characters.resolve_handle("Ada,").character->id, "ada");
    EXPECT_EQ(characters.resolve_handle("gr").character->id, "grace");
    EXPECT_EQ(characters.resolve_handle("?!").match, HandleMatch::unknown);
}

TEST(ForumCharacters, ResolvesUniqueWordsAndPrefixesInDisplayNames) {
    ForumCharacters characters({
        character("churchill", "Winston Churchill"),
        character("roosevelt", "Franklin Roosevelt"),
    });

    EXPECT_EQ(characters.resolve_handle("Winston").character->id, "churchill");
    EXPECT_EQ(characters.resolve_handle("Churchill").character->id, "churchill");
    EXPECT_EQ(characters.resolve_handle("Church").character->id, "churchill");
    EXPECT_EQ(characters.resolve_handle("Roose").character->id, "roosevelt");
}

TEST(ForumCharacters, ResolvesCharacterIdsThatTheDisplayNameCannotSpell) {
    ForumCharacters characters({
        character("markus_aurelius", "Marcus Aurelius"),
        character("stirlitz", "Штирлиц"),
    });

    EXPECT_EQ(characters.resolve_handle("markus_aurelius").character->id, "markus_aurelius");
    EXPECT_EQ(characters.resolve_handle("STIRLITZ").character->id, "stirlitz");
    EXPECT_EQ(characters.resolve_handle("stirlitz.").character->id, "stirlitz");
    EXPECT_EQ(characters.resolve_handle("Marcus").character->id, "markus_aurelius");
    EXPECT_EQ(characters.resolve_handle("markus").match, HandleMatch::unknown);
}

TEST(ForumCharacters, PrefersExactMatchesOverWordsAndPrefixes) {
    ForumCharacters characters({
        character("winston", "Franklin Roosevelt"),
        character("churchill", "Winston Churchill"),
    });

    // An exact display name beats another character's ID.
    EXPECT_EQ(characters.resolve_handle("Winston Churchill").character->id, "churchill");
    // An exact ID beats a word of another character's display name.
    EXPECT_EQ(characters.resolve_handle("Winston").character->id, "winston");
}

TEST(ForumCharacters, ReportsAmbiguousDisplayNameWords) {
    ForumCharacters characters({
        character("churchill", "Winston Churchill"),
        character("smith", "Winston Smith"),
    });

    const HandleResolution result = characters.resolve_handle("Winston");
    ASSERT_EQ(result.match, HandleMatch::ambiguous);
    ASSERT_EQ(result.candidates.size(), 2U);
}

TEST(ForumCharacters, RejectsAmbiguousPrefixAndUnusableNames) {
    ForumCharacters characters({character("ada", "Ada"), character("adam", "Adam")});
    EXPECT_EQ(characters.resolve_handle("a").match, HandleMatch::ambiguous);
    EXPECT_THROW(
        ForumCharacters({character("bad", " Local assistant")}),
        std::invalid_argument);
}

TEST(ForumCharacters, RejectsDuplicateAndInvalidCharacterIdentity) {
    EXPECT_THROW(ForumCharacters(std::vector<CharacterMetadata>{}), std::invalid_argument);
    EXPECT_THROW(
        ForumCharacters({character("same", "Ada"), character("same", "Grace")}),
        std::invalid_argument);
    EXPECT_THROW(
        ForumCharacters({character("one", "Ada"), character("two", "ada")}),
        std::invalid_argument);
    EXPECT_THROW(
        ForumCharacters({character("bad id", "Ada")}),
        std::invalid_argument);
    EXPECT_THROW(
        ForumCharacters({character("good", "@Ada")}),
        std::invalid_argument);
}

TEST(ForumCharacters, PrefersExactNamesAndReportsAllAmbiguousCandidates) {
    ForumCharacters characters({
        character("ada", "Ada"),
        character("adam", "Adam"),
        character("adrian", "Adrian"),
    });
    EXPECT_EQ(characters.resolve_handle("Ada").character->id, "ada");
    const HandleResolution ambiguous = characters.resolve_handle("ad");
    ASSERT_EQ(ambiguous.match, HandleMatch::ambiguous);
    ASSERT_EQ(ambiguous.candidates.size(), 3U);
    EXPECT_EQ(characters.handle_list(), "@Ada, @Adam, @Adrian");
}

TEST(ForumCharacters, ExposesTheFirstCharacterAndLooksUpImmutableIds) {
    const ForumCharacters characters({
        character("ada", "Ada"),
        character("grace", "Grace"),
    });

    EXPECT_EQ(characters.all().front().id, "ada");
    EXPECT_EQ(characters.all().front().display_name, "Ada");
    EXPECT_EQ(characters.all().size(), 2U);

    ASSERT_NE(characters.find("grace"), nullptr);
    EXPECT_EQ(characters.find("grace")->display_name, "Grace");
    EXPECT_EQ(characters.find("Grace"), nullptr)
        << "ids are matched exactly, not folded";
    EXPECT_EQ(characters.find("nobody"), nullptr);
    EXPECT_EQ(characters.find(""), nullptr);
}

TEST(ForumCharacters, SetsOneCharactersAppearanceInPlace) {
    ForumCharacters characters({
        character("ada", "Ada"),
        character("grace", "Grace"),
    });
    const CharacterAppearance styled{
        CharacterFont::sans, CharacterSlant::normal, CharacterWeight::bold,
        CharacterScale::normal};

    EXPECT_TRUE(characters.set_appearance("grace", styled));
    ASSERT_NE(characters.find("grace"), nullptr);
    EXPECT_EQ(characters.find("grace")->appearance, styled);
    EXPECT_EQ(characters.all().back().appearance, styled);
    EXPECT_EQ(characters.find("ada")->appearance, CharacterAppearance{})
        << "other characters are untouched";
}

TEST(ForumCharacters, RejectsAnAppearanceChangeForAnUnknownId) {
    ForumCharacters characters({character("ada", "Ada")});
    const CharacterAppearance styled{
        CharacterFont::serif, CharacterSlant::italic, CharacterWeight::normal,
        CharacterScale::large};

    EXPECT_FALSE(characters.set_appearance("nobody", styled));
    EXPECT_EQ(characters.find("ada")->appearance, CharacterAppearance{})
        << "an unknown ID changes nothing";
}

TEST(ForumCharacters, LeavesIdentityAndResolutionIntactAcrossAnAppearanceChange) {
    ForumCharacters characters({
        character("churchill", "Winston Churchill"),
        character("roosevelt", "Franklin Roosevelt"),
    });
    const CharacterAppearance styled{
        CharacterFont::mono, CharacterSlant::normal, CharacterWeight::normal,
        CharacterScale::small};

    ASSERT_TRUE(characters.set_appearance("churchill", styled));
    EXPECT_EQ(characters.all().size(), 2U);
    EXPECT_EQ(characters.all().front().id, "churchill");
    EXPECT_EQ(characters.all().front().display_name, "Winston Churchill");
    EXPECT_EQ(characters.resolve_handle("Winston").character->id, "churchill");
    EXPECT_EQ(characters.resolve_handle("Roose").character->id, "roosevelt");
}

TEST(ForumCharacters, KeepsAPunctuatedNameReachableWhileStillRetryingTrailingPunctuation) {
    // The ordering that justifies the retry design: verbatim wins over trimmed.
    const ForumCharacters characters({
        character("dotted", "Ismael."),
        character("plain", "Ismael"),
    });

    EXPECT_EQ(characters.resolve_handle("Ismael.").character->id, "dotted");
    EXPECT_EQ(characters.resolve_handle("ismael.").character->id, "dotted");
    EXPECT_EQ(characters.resolve_handle("Ismael").character->id, "plain");
    EXPECT_EQ(characters.resolve_handle("Ismael,").character->id, "plain");
}

TEST(ForumCharacters, ResolvesAPunctuatedHandleThroughAUniquePrefix) {
    const ForumCharacters characters({
        character("cheburashka", "Cheburashka"),
        character("ismael", "Ismael"),
    });

    EXPECT_EQ(characters.resolve_handle("Che,").character->id, "cheburashka");
    EXPECT_EQ(characters.resolve_handle("c").character->id, "cheburashka");
    EXPECT_EQ(characters.resolve_handle("Чебу").match, HandleMatch::unknown);
}

TEST(ForumCharacters, MatchesANonAsciiPrefixByBytes) {
    const ForumCharacters characters({
        character("cheb", "Чебурашка"),
        character("ismael", "Ismael"),
    });

    EXPECT_EQ(characters.resolve_handle("Чебурашка").character->id, "cheb");
    EXPECT_EQ(characters.resolve_handle("Чебу").character->id, "cheb");
}

TEST(ForumCharacters, TreatsEmptyAndUnknownHandlesAsUnresolved) {
    const ForumCharacters characters({character("ada", "Ada")});

    EXPECT_EQ(characters.resolve_handle("").match, HandleMatch::unknown);
    EXPECT_EQ(characters.resolve_handle(",").match, HandleMatch::unknown);
    EXPECT_EQ(characters.resolve_handle("nobody").match, HandleMatch::unknown);
    EXPECT_EQ(characters.resolve_handle("Ada").character->id, "ada");
    EXPECT_EQ(characters.handle_list(), "@Ada");
}

TEST(ForumCharacters, RejectsEveryUnusableDisplayNameForm) {
    EXPECT_THROW(ForumCharacters({character("good", "")}), std::invalid_argument);
    EXPECT_THROW(ForumCharacters({character("good", "/Ada")}), std::invalid_argument);
    EXPECT_THROW(ForumCharacters({character("good", "Ada ")}), std::invalid_argument);
    EXPECT_THROW(ForumCharacters({character("good", "Persona")}), std::invalid_argument);
    EXPECT_THROW(ForumCharacters({character("good", "persona")}), std::invalid_argument);
    EXPECT_THROW(ForumCharacters({character("", "Ada")}), std::invalid_argument);
    EXPECT_NO_THROW(ForumCharacters({character("good", "Personas")}));
    EXPECT_NO_THROW(ForumCharacters({character("good", "Winston Churchill")}));
}

} // namespace cha
