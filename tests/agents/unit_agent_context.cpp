#include "agents/agent.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace cha {
namespace {

std::vector<AgentMessage> context(
    const TranscriptSnapshot& transcript,
    std::string_view system_prompt,
    std::string_view agent_id) {
    return project_agent_context(
        transcript.entries,
        transcript.open_entry_id,
        system_prompt,
        agent_id);
}

TEST(AgentContext, ProjectsRolesFromKindsAndStableParticipantIds) {
    const TranscriptSnapshot transcript{
        .entries = {
            make_human_entry(1, "reviewer-id", "Reviewer", "Draft an answer", 1),
            make_agent_entry(
                2, "writer-id", "You", "Initial draft", EntryStatus::complete, 1),
            make_human_entry(3, "reviewer-id", "Reviewer", "Review it", 2),
            make_agent_entry(
                4, "reviewer-id", "System", "Looks good", EntryStatus::complete, 2),
        },
    };

    EXPECT_EQ(
        context(transcript, "Be concise.", "reviewer-id"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "Be concise."},
            {AgentRole::user, "User: Draft an answer\n\nYou: Initial draft\n\nUser: Review it"},
            {AgentRole::assistant, "Looks good"},
        }));
}

TEST(AgentContext, OmitsNoticesErrorsFailedPromptsAndIncompleteAgentEntries) {
    const TranscriptSnapshot transcript{
        .entries = {
            make_human_entry(1, "reviewer-id", "Reviewer", "Failed request", 7),
            make_error_entry(2, "unavailable", 7, "reviewer-id"),
            make_notice_entry(3, "Model information"),
            make_human_entry(4, "reviewer-id", "Reviewer", "Current request", 8),
            make_agent_entry(
                5, "reviewer-id", "Reviewer", "Stopped", EntryStatus::cancelled, 8),
            make_agent_entry(
                6, "reviewer-id", "Reviewer", "Partial", EntryStatus::streaming, 8),
        },
        .open_entry_id = 6,
    };

    EXPECT_EQ(
        context(transcript, {}, "reviewer-id"),
        (std::vector<AgentMessage>{{AgentRole::user, "Current request"}}));
}

TEST(AgentContext, DisplayNameChangesDoNotChangeAgentRole) {
    TranscriptSnapshot before{
        .entries = {
            make_agent_entry(
                1, "stable-id", "Old name", "Answer", EntryStatus::complete, 1),
        },
    };
    TranscriptSnapshot after = before;
    after.entries.front().display_name = "New name";

    EXPECT_EQ(
        context(before, {}, "stable-id"),
        (std::vector<AgentMessage>{{AgentRole::assistant, "Answer"}}));
    EXPECT_EQ(
        context(after, {}, "stable-id"),
        (std::vector<AgentMessage>{{AgentRole::assistant, "Answer"}}));
}

TEST(AgentContext, PreservesTheSingleAgentWireShapeByteForByte) {
    const TranscriptSnapshot transcript{
        .entries = {
            make_human_entry(1, "assistant", "Assistant", "First", 1),
            make_agent_entry(2, "assistant", "Assistant", "Answer", EntryStatus::complete, 1),
            make_human_entry(3, "assistant", "Assistant", "Second", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "assistant"),
        (std::vector<AgentMessage>{
            {AgentRole::user, "First"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Second"},
        }));
}

TEST(AgentContext, KeepsAdjacentHumanPromptsSeparateAfterCancelledOutput) {
    const TranscriptSnapshot transcript{
        .entries = {
            make_human_entry(1, "assistant", "Assistant", "First", 1),
            make_agent_entry(2, "assistant", "Assistant", "Partial", EntryStatus::cancelled, 1),
            make_human_entry(3, "assistant", "Assistant", "Second", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "assistant"),
        (std::vector<AgentMessage>{
            {AgentRole::user, "First"},
            {AgentRole::user, "Second"},
        }));
}

// Reproduces the two-agent transcript of the design proposal's worked example.
TranscriptSnapshot lobby_transcript() {
    return {
        .entries = {
            make_human_entry(1, "cheburashka", "Cheburashka", "Who are you?", 1),
            make_agent_entry(
                2, "cheburashka", "Cheburashka", "I am Cheburashka.",
                EntryStatus::complete, 1),
            make_human_entry(3, "ismael", "Ismael", "And you?", 2),
            make_agent_entry(
                4, "ismael", "Ismael", "Call me Ismael.", EntryStatus::complete, 2),
            make_human_entry(5, "cheburashka", "Cheburashka", "What did he say?", 3),
        },
    };
}

TEST(AgentContext, ProjectsTheSharedTranscriptForTheAddressedAgent) {
    EXPECT_EQ(
        context(lobby_transcript(), "Cheburashka system", "cheburashka"),
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

TEST(AgentContext, ProjectsTheSameTranscriptFromTheOtherAgentsPointOfView) {
    EXPECT_EQ(
        context(lobby_transcript(), "Ismael system", "ismael"),
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
        context(lobby_transcript(), {}, "cheburashka");

    ASSERT_EQ(projected.size(), 3U);
    // Rule 2 of the projection: "User: " always, "[to X] " only for foreign targets.
    EXPECT_EQ(projected.front().content, "User: Who are you?");
    EXPECT_EQ(projected.front().content.find("[to "), std::string::npos);
    EXPECT_NE(projected.back().content.find("User: [to Ismael] And you?"), std::string::npos);
    EXPECT_NE(projected.back().content.find("\n\nUser: What did he say?"), std::string::npos);
    EXPECT_EQ(projected.back().content.find("[to Cheburashka]"), std::string::npos);
}

TEST(AgentContext, OmitsTheAddressingMarkerEntirelyWithOneParticipant) {
    const TranscriptSnapshot transcript{
        .entries = {
            make_human_entry(1, "ismael", "Ismael", "Who are you?", 1),
            make_agent_entry(
                2, "ismael", "Ismael", "Call me Ismael.", EntryStatus::complete, 1),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "ismael"),
        (std::vector<AgentMessage>{
            {AgentRole::user, "Who are you?"},
            {AgentRole::assistant, "Call me Ismael."},
        }));
}

TEST(AgentContext, AttributesAgentsWhosePersonasAreNoLongerInTheRoom) {
    const TranscriptSnapshot transcript{
        .entries = {
            make_human_entry(1, "departed", "Departed", "Say something", 1),
            make_agent_entry(
                2, "departed", "Departed", "Farewell", EntryStatus::complete, 1),
            make_human_entry(3, "ismael", "Ismael", "Your turn", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "ismael"),
        (std::vector<AgentMessage>{
            {AgentRole::user,
             "User: [to Departed] Say something"
             "\n\nDeparted: Farewell"
             "\n\nUser: Your turn"},
        }));
}

} // namespace
} // namespace cha
