#include "agent_context.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(AgentContext, ProjectsRolesFromKindsAndStableParticipantIds) {
    const ConversationSnapshot conversation{
        .entries = {
            make_human_entry(1, "Draft an answer", 1),
            make_agent_entry(
                2, "writer-id", "You", "Initial draft", CompletionStatus::complete, 1),
            make_human_entry(3, "Review it", 2),
            make_agent_entry(
                4, "reviewer-id", "System", "Looks good", CompletionStatus::complete, 2),
        },
    };

    EXPECT_EQ(
        build_agent_context(conversation, "Be concise.", "reviewer-id"),
        (std::vector<AgentMessage>{
            {"system", "Be concise."},
            {"user", "Draft an answer"},
            {"assistant", "writer-id: Initial draft"},
            {"user", "Review it"},
            {"assistant", "Looks good"},
        }));
}

TEST(AgentContext, OmitsNoticesErrorsFailedPromptsAndIncompleteAgentEntries) {
    const ConversationSnapshot conversation{
        .entries = {
            make_human_entry(1, "Failed request", 7),
            make_error_entry(2, "unavailable", 7, "reviewer-id"),
            make_notice_entry(3, "Model information"),
            make_human_entry(4, "Current request", 8),
            make_agent_entry(
                5, "reviewer-id", "Reviewer", "Stopped", CompletionStatus::cancelled, 8),
            make_agent_entry(
                6, "reviewer-id", "Reviewer", "Partial", CompletionStatus::streaming, 8),
        },
        .open_entry_id = 6,
    };

    EXPECT_EQ(
        build_agent_context(conversation, {}, "reviewer-id"),
        (std::vector<AgentMessage>{{"user", "Current request"}}));
}

TEST(AgentContext, DisplayNameChangesDoNotChangeAgentRole) {
    ConversationSnapshot before{
        .entries = {
            make_agent_entry(
                1, "stable-id", "Old name", "Answer", CompletionStatus::complete, 1),
        },
    };
    ConversationSnapshot after = before;
    after.entries.front().display_name = "New name";

    EXPECT_EQ(build_agent_context(before, {}, "stable-id"), (std::vector<AgentMessage>{{"assistant", "Answer"}}));
    EXPECT_EQ(build_agent_context(after, {}, "stable-id"), (std::vector<AgentMessage>{{"assistant", "Answer"}}));
}

} // namespace
} // namespace cha
