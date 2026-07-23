#include "agent_roster.h"

#include <gtest/gtest.h>

namespace cha {
namespace {
AgentInfo agent(std::string id, std::string name) { return {std::move(id), std::move(name), "m", "api", true}; }
}

TEST(AgentRoster, ResolvesNamesPunctuationAndUniquePrefixes) {
    AgentRoster roster({agent("ada", "Ada"), agent("grace", "Grace")});
    EXPECT_EQ(roster.resolve_handle("ADA").agent->id, "ada");
    EXPECT_EQ(roster.resolve_handle("Ada,").agent->id, "ada");
    EXPECT_EQ(roster.resolve_handle("gr").agent->id, "grace");
    EXPECT_EQ(roster.resolve_handle("?!").match, HandleMatch::unknown);
}

TEST(AgentRoster, RejectsAmbiguousPrefixAndUnusableNames) {
    AgentRoster roster({agent("ada", "Ada"), agent("adam", "Adam")});
    EXPECT_EQ(roster.resolve_handle("a").match, HandleMatch::ambiguous);
    EXPECT_THROW(AgentRoster({agent("bad", "Local assistant")}), std::invalid_argument);
}

TEST(AgentRoster, RejectsDuplicateAndInvalidDirectBackendMetadata) {
    EXPECT_THROW(AgentRoster(std::vector<AgentInfo>{}), std::invalid_argument);
    EXPECT_THROW(
        AgentRoster({agent("same", "Ada"), agent("same", "Grace")}),
        std::invalid_argument);
    EXPECT_THROW(
        AgentRoster({agent("one", "Ada"), agent("two", "ada")}),
        std::invalid_argument);
    EXPECT_THROW(AgentRoster({agent("bad id", "Ada")}), std::invalid_argument);
    EXPECT_THROW(AgentRoster({agent("good", "@Ada")}), std::invalid_argument);
}

TEST(AgentRoster, PrefersExactNamesAndReportsAllAmbiguousCandidates) {
    AgentRoster roster({agent("ada", "Ada"), agent("adam", "Adam"), agent("adrian", "Adrian")});
    EXPECT_EQ(roster.resolve_handle("Ada").agent->id, "ada");
    const HandleResolution ambiguous = roster.resolve_handle("ad");
    ASSERT_EQ(ambiguous.match, HandleMatch::ambiguous);
    ASSERT_EQ(ambiguous.candidates.size(), 3U);
    EXPECT_EQ(roster.handle_list(), "@Ada, @Adam, @Adrian");
}

TEST(AgentRoster, ExposesTheDefaultAgentAndLooksUpImmutableIds) {
    const AgentRoster roster({agent("ada", "Ada"), agent("grace", "Grace")});

    EXPECT_EQ(roster.first().id, "ada");
    EXPECT_EQ(roster.first().name, "Ada");
    EXPECT_EQ(roster.agents().size(), 2U);

    ASSERT_NE(roster.find("grace"), nullptr);
    EXPECT_EQ(roster.find("grace")->name, "Grace");
    EXPECT_EQ(roster.find("Grace"), nullptr) << "ids are matched exactly, not folded";
    EXPECT_EQ(roster.find("nobody"), nullptr);
    EXPECT_EQ(roster.find(""), nullptr);
}

TEST(AgentRoster, KeepsAPunctuatedNameReachableWhileStillRetryingTrailingPunctuation) {
    // The ordering that justifies the retry design: verbatim wins over trimmed.
    const AgentRoster roster({agent("dotted", "Ismael."), agent("plain", "Ismael")});

    EXPECT_EQ(roster.resolve_handle("Ismael.").agent->id, "dotted");
    EXPECT_EQ(roster.resolve_handle("ismael.").agent->id, "dotted");
    EXPECT_EQ(roster.resolve_handle("Ismael").agent->id, "plain");
    EXPECT_EQ(roster.resolve_handle("Ismael,").agent->id, "plain");
}

TEST(AgentRoster, ResolvesAPunctuatedHandleThroughAUniquePrefix) {
    const AgentRoster roster({agent("cheburashka", "Cheburashka"), agent("ismael", "Ismael")});

    EXPECT_EQ(roster.resolve_handle("Che,").agent->id, "cheburashka");
    EXPECT_EQ(roster.resolve_handle("c").agent->id, "cheburashka");
    EXPECT_EQ(roster.resolve_handle("Чебу").match, HandleMatch::unknown);
}

TEST(AgentRoster, MatchesANonAsciiPrefixByBytes) {
    const AgentRoster roster({agent("cheb", "Чебурашка"), agent("ismael", "Ismael")});

    EXPECT_EQ(roster.resolve_handle("Чебурашка").agent->id, "cheb");
    EXPECT_EQ(roster.resolve_handle("Чебу").agent->id, "cheb");
}

TEST(AgentRoster, TreatsEmptyAndUnknownHandlesAsUnresolved) {
    const AgentRoster roster({agent("ada", "Ada")});

    EXPECT_EQ(roster.resolve_handle("").match, HandleMatch::unknown);
    EXPECT_EQ(roster.resolve_handle(",").match, HandleMatch::unknown);
    EXPECT_EQ(roster.resolve_handle("nobody").match, HandleMatch::unknown);
    EXPECT_EQ(roster.resolve_handle("Ada").agent->id, "ada");
    EXPECT_EQ(roster.handle_list(), "@Ada");
}

TEST(AgentRoster, RejectsEveryUnusableDisplayNameForm) {
    EXPECT_THROW(AgentRoster({agent("good", "")}), std::invalid_argument);
    EXPECT_THROW(AgentRoster({agent("good", "/Ada")}), std::invalid_argument);
    EXPECT_THROW(AgentRoster({agent("good", "User")}), std::invalid_argument);
    EXPECT_THROW(AgentRoster({agent("good", "user")}), std::invalid_argument);
    EXPECT_THROW(AgentRoster({agent("", "Ada")}), std::invalid_argument);
    EXPECT_NO_THROW(AgentRoster({agent("good", "Users")}));
}

} // namespace cha
