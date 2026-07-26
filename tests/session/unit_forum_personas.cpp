#include "session/forum_personas.h"

#include <gtest/gtest.h>

namespace cha {
namespace {
PersonaInfo persona(std::string id, std::string name) {
    return {std::move(id), std::move(name)};
}
}

TEST(ForumPersonas, ResolvesNamesPunctuationAndUniquePrefixes) {
    ForumPersonas personas({persona("ada", "Ada"), persona("grace", "Grace")});
    EXPECT_EQ(personas.resolve_handle("ADA").persona->id, "ada");
    EXPECT_EQ(personas.resolve_handle("Ada,").persona->id, "ada");
    EXPECT_EQ(personas.resolve_handle("gr").persona->id, "grace");
    EXPECT_EQ(personas.resolve_handle("?!").match, HandleMatch::unknown);
}

TEST(ForumPersonas, RejectsAmbiguousPrefixAndUnusableNames) {
    ForumPersonas personas({persona("ada", "Ada"), persona("adam", "Adam")});
    EXPECT_EQ(personas.resolve_handle("a").match, HandleMatch::ambiguous);
    EXPECT_THROW(
        ForumPersonas({persona("bad", "Local assistant")}),
        std::invalid_argument);
}

TEST(ForumPersonas, RejectsDuplicateAndInvalidPersonaIdentity) {
    EXPECT_THROW(ForumPersonas(std::vector<PersonaInfo>{}), std::invalid_argument);
    EXPECT_THROW(
        ForumPersonas({persona("same", "Ada"), persona("same", "Grace")}),
        std::invalid_argument);
    EXPECT_THROW(
        ForumPersonas({persona("one", "Ada"), persona("two", "ada")}),
        std::invalid_argument);
    EXPECT_THROW(
        ForumPersonas({persona("bad id", "Ada")}),
        std::invalid_argument);
    EXPECT_THROW(
        ForumPersonas({persona("good", "@Ada")}),
        std::invalid_argument);
}

TEST(ForumPersonas, PrefersExactNamesAndReportsAllAmbiguousCandidates) {
    ForumPersonas personas({
        persona("ada", "Ada"),
        persona("adam", "Adam"),
        persona("adrian", "Adrian"),
    });
    EXPECT_EQ(personas.resolve_handle("Ada").persona->id, "ada");
    const HandleResolution ambiguous = personas.resolve_handle("ad");
    ASSERT_EQ(ambiguous.match, HandleMatch::ambiguous);
    ASSERT_EQ(ambiguous.candidates.size(), 3U);
    EXPECT_EQ(personas.handle_list(), "@Ada, @Adam, @Adrian");
}

TEST(ForumPersonas, ExposesTheFirstPersonaAndLooksUpImmutableIds) {
    const ForumPersonas personas({
        persona("ada", "Ada"),
        persona("grace", "Grace"),
    });

    EXPECT_EQ(personas.first().id, "ada");
    EXPECT_EQ(personas.first().name, "Ada");
    EXPECT_EQ(personas.all().size(), 2U);

    ASSERT_NE(personas.find("grace"), nullptr);
    EXPECT_EQ(personas.find("grace")->name, "Grace");
    EXPECT_EQ(personas.find("Grace"), nullptr)
        << "ids are matched exactly, not folded";
    EXPECT_EQ(personas.find("nobody"), nullptr);
    EXPECT_EQ(personas.find(""), nullptr);
}

TEST(ForumPersonas, KeepsAPunctuatedNameReachableWhileStillRetryingTrailingPunctuation) {
    // The ordering that justifies the retry design: verbatim wins over trimmed.
    const ForumPersonas personas({
        persona("dotted", "Ismael."),
        persona("plain", "Ismael"),
    });

    EXPECT_EQ(personas.resolve_handle("Ismael.").persona->id, "dotted");
    EXPECT_EQ(personas.resolve_handle("ismael.").persona->id, "dotted");
    EXPECT_EQ(personas.resolve_handle("Ismael").persona->id, "plain");
    EXPECT_EQ(personas.resolve_handle("Ismael,").persona->id, "plain");
}

TEST(ForumPersonas, ResolvesAPunctuatedHandleThroughAUniquePrefix) {
    const ForumPersonas personas({
        persona("cheburashka", "Cheburashka"),
        persona("ismael", "Ismael"),
    });

    EXPECT_EQ(personas.resolve_handle("Che,").persona->id, "cheburashka");
    EXPECT_EQ(personas.resolve_handle("c").persona->id, "cheburashka");
    EXPECT_EQ(personas.resolve_handle("Чебу").match, HandleMatch::unknown);
}

TEST(ForumPersonas, MatchesANonAsciiPrefixByBytes) {
    const ForumPersonas personas({
        persona("cheb", "Чебурашка"),
        persona("ismael", "Ismael"),
    });

    EXPECT_EQ(personas.resolve_handle("Чебурашка").persona->id, "cheb");
    EXPECT_EQ(personas.resolve_handle("Чебу").persona->id, "cheb");
}

TEST(ForumPersonas, TreatsEmptyAndUnknownHandlesAsUnresolved) {
    const ForumPersonas personas({persona("ada", "Ada")});

    EXPECT_EQ(personas.resolve_handle("").match, HandleMatch::unknown);
    EXPECT_EQ(personas.resolve_handle(",").match, HandleMatch::unknown);
    EXPECT_EQ(personas.resolve_handle("nobody").match, HandleMatch::unknown);
    EXPECT_EQ(personas.resolve_handle("Ada").persona->id, "ada");
    EXPECT_EQ(personas.handle_list(), "@Ada");
}

TEST(ForumPersonas, RejectsEveryUnusableDisplayNameForm) {
    EXPECT_THROW(ForumPersonas({persona("good", "")}), std::invalid_argument);
    EXPECT_THROW(ForumPersonas({persona("good", "/Ada")}), std::invalid_argument);
    EXPECT_THROW(ForumPersonas({persona("good", "User")}), std::invalid_argument);
    EXPECT_THROW(ForumPersonas({persona("good", "user")}), std::invalid_argument);
    EXPECT_THROW(ForumPersonas({persona("", "Ada")}), std::invalid_argument);
    EXPECT_NO_THROW(ForumPersonas({persona("good", "Users")}));
}

} // namespace cha
