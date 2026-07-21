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

} // namespace cha
