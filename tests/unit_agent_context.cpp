#include "agent_context.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cha {
namespace {

std::vector<AgentMessage> context(
    const ConversationSnapshot& conversation,
    std::string_view system_prompt,
    std::string_view agent_id) {
    return project_agent_context(
        conversation.entries,
        conversation.open_entry_id,
        system_prompt,
        agent_id);
}

TEST(AgentContext, ProjectsRolesFromKindsAndStableParticipantIds) {
    const ConversationSnapshot conversation{
        .entries = {
            make_human_entry(1, "reviewer-id", "Reviewer", "Draft an answer", 1),
            make_agent_entry(
                2, "writer-id", "You", "Initial draft", CompletionStatus::complete, 1),
            make_human_entry(3, "reviewer-id", "Reviewer", "Review it", 2),
            make_agent_entry(
                4, "reviewer-id", "System", "Looks good", CompletionStatus::complete, 2),
        },
    };

    EXPECT_EQ(
        context(conversation, "Be concise.", "reviewer-id"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "Be concise."},
            {AgentRole::user, "User: Draft an answer\n\nYou: Initial draft\n\nUser: Review it"},
            {AgentRole::assistant, "Looks good"},
        }));
}

TEST(AgentContext, OmitsNoticesErrorsFailedPromptsAndIncompleteAgentEntries) {
    const ConversationSnapshot conversation{
        .entries = {
            make_human_entry(1, "reviewer-id", "Reviewer", "Failed request", 7),
            make_error_entry(2, "unavailable", 7, "reviewer-id"),
            make_notice_entry(3, "Model information"),
            make_human_entry(4, "reviewer-id", "Reviewer", "Current request", 8),
            make_agent_entry(
                5, "reviewer-id", "Reviewer", "Stopped", CompletionStatus::cancelled, 8),
            make_agent_entry(
                6, "reviewer-id", "Reviewer", "Partial", CompletionStatus::streaming, 8),
        },
        .open_entry_id = 6,
    };

    EXPECT_EQ(
        context(conversation, {}, "reviewer-id"),
        (std::vector<AgentMessage>{{AgentRole::user, "Current request"}}));
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

    EXPECT_EQ(
        context(before, {}, "stable-id"),
        (std::vector<AgentMessage>{{AgentRole::assistant, "Answer"}}));
    EXPECT_EQ(
        context(after, {}, "stable-id"),
        (std::vector<AgentMessage>{{AgentRole::assistant, "Answer"}}));
}

TEST(AgentContext, ExcludesOwnAndForeignAgentReasoning) {
    const ConversationSnapshot conversation{
        .entries = {
            make_agent_entry(
                1,
                "reviewer-id",
                "Reviewer",
                {.reasoning = "PRIVATE_OWN_REASONING", .answer = "Own answer"},
                CompletionStatus::complete,
                1),
            make_agent_entry(
                2,
                "writer-id",
                "Writer",
                {.reasoning = "PRIVATE_FOREIGN_REASONING", .answer = "Foreign answer"},
                CompletionStatus::complete,
                2),
        },
    };

    const std::vector<AgentMessage> projected =
        context(conversation, {}, "reviewer-id");
    ASSERT_EQ(projected.size(), 2U);
    EXPECT_EQ(projected[0], (AgentMessage{AgentRole::assistant, "Own answer"}));
    EXPECT_EQ(
        projected[1],
        (AgentMessage{AgentRole::user, "Writer: Foreign answer"}));
    for (const AgentMessage& message : projected) {
        EXPECT_EQ(message.content.find("PRIVATE_"), std::string::npos);
    }
}

TEST(AgentContext, PreservesTheSingleAgentWireShapeByteForByte) {
    const ConversationSnapshot conversation{
        .entries = {
            make_human_entry(1, "assistant", "Assistant", "First", 1),
            make_agent_entry(2, "assistant", "Assistant", "Answer", CompletionStatus::complete, 1),
            make_human_entry(3, "assistant", "Assistant", "Second", 2),
        },
    };

    EXPECT_EQ(
        context(conversation, {}, "assistant"),
        (std::vector<AgentMessage>{
            {AgentRole::user, "First"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Second"},
        }));
}

TEST(AgentContext, KeepsAdjacentHumanPromptsSeparateAfterCancelledOutput) {
    const ConversationSnapshot conversation{
        .entries = {
            make_human_entry(1, "assistant", "Assistant", "First", 1),
            make_agent_entry(2, "assistant", "Assistant", "Partial", CompletionStatus::cancelled, 1),
            make_human_entry(3, "assistant", "Assistant", "Second", 2),
        },
    };

    EXPECT_EQ(
        context(conversation, {}, "assistant"),
        (std::vector<AgentMessage>{
            {AgentRole::user, "First"},
            {AgentRole::user, "Second"},
        }));
}

// Reproduces the two-agent transcript of the design proposal's worked example.
ConversationSnapshot lobby_conversation() {
    return {
        .entries = {
            make_human_entry(1, "cheburashka", "Cheburashka", "Who are you?", 1),
            make_agent_entry(
                2, "cheburashka", "Cheburashka", "I am Cheburashka.",
                CompletionStatus::complete, 1),
            make_human_entry(3, "ismael", "Ismael", "And you?", 2),
            make_agent_entry(
                4, "ismael", "Ismael", "Call me Ismael.", CompletionStatus::complete, 2),
            make_human_entry(5, "cheburashka", "Cheburashka", "What did he say?", 3),
        },
    };
}

TEST(AgentContext, ProjectsTheSharedConversationForTheAddressedAgent) {
    EXPECT_EQ(
        context(lobby_conversation(), "Cheburashka system", "cheburashka"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "Cheburashka system"},
            {AgentRole::user, "User: Who are you?"},
            {AgentRole::assistant, "I am Cheburashka."},
            {AgentRole::user,
             "User: [to Ismael] And you?"
             "\n\nIsmael: Call me Ismael."
             "\n\nUser: What did he say?"},
        }));
}

TEST(AgentContext, ProjectsTheSameConversationFromTheOtherAgentsPointOfView) {
    EXPECT_EQ(
        context(lobby_conversation(), "Ismael system", "ismael"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "Ismael system"},
            {AgentRole::user,
             "User: [to Cheburashka] Who are you?"
             "\n\nCheburashka: I am Cheburashka."
             "\n\nUser: And you?"},
            {AgentRole::assistant, "Call me Ismael."},
            {AgentRole::user, "User: [to Cheburashka] What did he say?"},
        }));
}

TEST(AgentContext, MarksOnlyPromptsAddressedToSomebodyElse) {
    const std::vector<AgentMessage> projected =
        context(lobby_conversation(), {}, "cheburashka");

    ASSERT_EQ(projected.size(), 3U);
    // Rule 2 of the projection: "User: " always, "[to X] " only for foreign targets.
    EXPECT_EQ(projected.front().content, "User: Who are you?");
    EXPECT_EQ(projected.front().content.find("[to "), std::string::npos);
    EXPECT_NE(projected.back().content.find("User: [to Ismael] And you?"), std::string::npos);
    EXPECT_NE(projected.back().content.find("\n\nUser: What did he say?"), std::string::npos);
    EXPECT_EQ(projected.back().content.find("[to Cheburashka]"), std::string::npos);
}

TEST(AgentContext, OmitsTheAddressingMarkerEntirelyWithOneParticipant) {
    const ConversationSnapshot conversation{
        .entries = {
            make_human_entry(1, "ismael", "Ismael", "Who are you?", 1),
            make_agent_entry(
                2, "ismael", "Ismael", "Call me Ismael.", CompletionStatus::complete, 1),
        },
    };

    EXPECT_EQ(
        context(conversation, {}, "ismael"),
        (std::vector<AgentMessage>{
            {AgentRole::user, "Who are you?"},
            {AgentRole::assistant, "Call me Ismael."},
        }));
}

TEST(AgentContext, AttributesAgentsThatNoRosterStillContains) {
    const ConversationSnapshot conversation{
        .entries = {
            make_human_entry(1, "departed", "Departed", "Say something", 1),
            make_agent_entry(
                2, "departed", "Departed", "Farewell", CompletionStatus::complete, 1),
            make_human_entry(3, "ismael", "Ismael", "Your turn", 2),
        },
    };

    EXPECT_EQ(
        context(conversation, {}, "ismael"),
        (std::vector<AgentMessage>{
            {AgentRole::user,
             "User: [to Departed] Say something"
             "\n\nDeparted: Farewell"
             "\n\nUser: Your turn"},
        }));
}

} // namespace
} // namespace cha
