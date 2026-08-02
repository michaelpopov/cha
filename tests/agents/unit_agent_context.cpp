#include "agents/agent.h"
#include "support/test_transcript.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

constexpr std::string_view shared_history_header =
    "Shared chat history (JSONL):\n";

std::vector<AgentMessage> context(
    const CompletionHistory& transcript,
    std::string_view system_prompt,
    std::string_view agent_id) {
    return project_agent_context(
        transcript.entries,
        transcript.open_entry_id,
        {},
        system_prompt,
        agent_id);
}

AgentMessage human(std::string_view text) {
    return {AgentRole::persona, "from You:\n" + std::string(text)};
}

TEST(AgentContext, ProjectsRolesFromKindsAndStableParticipantIds) {
    const CompletionHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"reviewer-id", "Reviewer"}, "Draft an answer", 1),
            make_agent_entry(
                2, "writer-id", "You", "Initial draft", EntryStatus::complete, 1),
            test::human_entry(3, {"human", "You"}, {"reviewer-id", "Reviewer"}, "Review it", 2),
            make_agent_entry(
                4, "reviewer-id", "System", "Looks good", EntryStatus::complete, 2),
        },
    };

    EXPECT_EQ(
        context(transcript, "Be concise.", "reviewer-id"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "Be concise."},
            human("Draft an answer"),
            {AgentRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"agent","speaker":"You","text":"Initial draft"})"},
            human("Review it"),
            {AgentRole::assistant, "Looks good"},
        }));
}

TEST(AgentContext, OmitsNoticesErrorsFailedPromptsAndIncompleteAgentEntries) {
    const CompletionHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"reviewer-id", "Reviewer"}, "Failed request", 7),
            make_error_entry(2, "unavailable", 7, "reviewer-id"),
            make_notice_entry(3, "Model information"),
            test::human_entry(4, {"human", "You"}, {"reviewer-id", "Reviewer"}, "Current request", 8),
            make_agent_entry(
                5, "reviewer-id", "Reviewer", "Stopped", EntryStatus::cancelled, 8),
            make_agent_entry(
                6, "reviewer-id", "Reviewer", "Partial", EntryStatus::streaming, 8),
        },
        .open_entry_id = 6,
    };

    EXPECT_EQ(
        context(transcript, {}, "reviewer-id"),
        (std::vector<AgentMessage>{human("Current request")}));
}

TEST(AgentContext, OmitsTheClosedOffrecordSpan) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Before", 1),
        make_hide_on_marker(2),
        test::human_entry(3, {"human", "You"}, {"assistant", "Assistant"}, "Hidden", 2),
        make_agent_entry(4, "assistant", "Assistant", "Hidden answer", EntryStatus::complete, 2),
        make_hide_marker(5),
        test::human_entry(6, {"human", "You"}, {"assistant", "Assistant"}, "After", 3),
    };

    EXPECT_EQ(
        project_agent_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 2, .end = 5},
            {},
            "assistant"),
        (std::vector<AgentMessage>{
            human("Before"),
            human("After"),
        }));
}

TEST(AgentContext, AnOpenOffrecordSpanExcludesNothing) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Before", 1),
        make_hide_on_marker(2),
        test::human_entry(3, {"human", "You"}, {"assistant", "Assistant"}, "After opening", 2),
    };

    EXPECT_EQ(
        project_agent_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 2, .end = std::nullopt},
            {},
            "assistant"),
        (std::vector<AgentMessage>{
            human("Before"),
            human("After opening"),
        }));
}

TEST(AgentContext, ProjectsOnlyEntriesOutsideAClosedOffrecordSpan) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"one", "One"}, "Question", 1),
        make_agent_entry(2, "one", "One", "One answer", EntryStatus::complete, 1),
        test::human_entry(3, {"human", "You"}, {"two", "Two"}, "Question", 2),
    };

    EXPECT_EQ(
        project_agent_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 1, .end = 3},
            {},
            "two"),
        (std::vector<AgentMessage>{human("Question")}));
}

TEST(AgentContext, ImmutableInputKeepsATrailingSharedBlockSeparateFromPrompt) {
    auto history = std::make_shared<const CompletionHistory>(
        CompletionHistory{
            .entries = {
                test::human_entry(
                    1, {"human", "You"}, {"other", "Other"}, "Other question", 1),
                make_agent_entry(
                    2,
                    "other",
                    "Other",
                    "Other answer",
                    EntryStatus::complete,
                    1),
            },
        });
    const CompletionInput input{
        .history = std::move(history),
        .run = {
            .request_id = 2,
            .target = {"assistant", "Assistant"},
            .author = {"human", "You"},
            .prompt_text = "Current question",
        },
    };
    std::vector<TranscriptEntry> old_tail = input.history->entries;
    old_tail.push_back(test::human_entry(
        3, {"human", "You"}, {"assistant", "Assistant"}, "Current question", 2));

    const std::vector<AgentMessage> projected =
        project_agent_context(input, {});

    EXPECT_EQ(
        projected,
        project_agent_context(
            old_tail, std::nullopt, {}, {}, "assistant"));
    EXPECT_EQ(
        projected,
        (std::vector<AgentMessage>{
            {AgentRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Other","text":"Other question"})"
             "\n"
             R"({"kind":"agent","speaker":"Other","text":"Other answer"})"},
            human("Current question"),
        }));
}

TEST(AgentContext, PrefixesBothReplayedAndLivePromptsWithTheirOwnAuthors) {
    const CompletionHistory history{
        .entries = {
            test::human_entry(
                1, {"reader", "Reader"}, {"assistant", "Assistant"},
                "Earlier prompt", 1),
        },
    };
    const CompletionInput input{
        .history = std::make_shared<const CompletionHistory>(history),
        .run = {
            .request_id = 2,
            .target = {"assistant", "Assistant"},
            .author = {"athlete", "Athlete"},
            .prompt_text = "Current prompt",
        },
    };

    EXPECT_EQ(
        project_agent_context(input, {}),
        (std::vector<AgentMessage>{
            {AgentRole::persona, "from Reader:\nEarlier prompt"},
            {AgentRole::persona, "from Athlete:\nCurrent prompt"},
        }));
}

TEST(AgentContext, SplicesHiddenTurnsOutOfOneSharedHistoryBlock) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"other", "Other"}, "First question", 1),
        make_agent_entry(2, "other", "Other", "First answer", EntryStatus::complete, 1),
        make_hide_on_marker(3),
        test::human_entry(4, {"human", "You"}, {"assistant", "Assistant"}, "Hidden question", 2),
        make_agent_entry(5, "assistant", "Assistant", "Hidden answer", EntryStatus::complete, 2),
        make_hide_marker(6),
        test::human_entry(7, {"human", "You"}, {"other", "Other"}, "Second question", 3),
        make_agent_entry(8, "other", "Other", "Second answer", EntryStatus::complete, 3),
        test::human_entry(9, {"human", "You"}, {"assistant", "Assistant"}, "Current", 4),
    };

    EXPECT_EQ(
        project_agent_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 3, .end = 6},
            {},
            "assistant"),
        (std::vector<AgentMessage>{
            {AgentRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Other","text":"First question"})"
             "\n"
             R"({"kind":"agent","speaker":"Other","text":"First answer"})"
             "\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Other","text":"Second question"})"
             "\n"
             R"({"kind":"agent","speaker":"Other","text":"Second answer"})"},
            human("Current"),
        }));
}

TEST(AgentContext, SpanCanHideAllEarlierTurnsWithoutHidingTheCurrentPrompt) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Earlier", 1),
        make_agent_entry(2, "assistant", "Assistant", "Earlier answer", EntryStatus::complete, 1),
        make_hide_on_marker(3),
        make_hide_marker(4),
        test::human_entry(5, {"human", "You"}, {"assistant", "Assistant"}, "Current", 2),
    };

    EXPECT_EQ(
        project_agent_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 1, .end = 5},
            "System",
            "assistant"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "System"},
            human("Current"),
        }));
}

TEST(AgentContext, CombinesSpanExclusionWithFailedAndCancelledTurnRules) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Hidden", 1),
        test::human_entry(2, {"human", "You"}, {"assistant", "Assistant"}, "Failed", 2),
        make_error_entry(3, "unavailable", 2, "assistant"),
        test::human_entry(4, {"human", "You"}, {"assistant", "Assistant"}, "Cancelled", 3),
        make_agent_entry(5, "assistant", "Assistant", "Partial", EntryStatus::cancelled, 3),
        test::human_entry(6, {"human", "You"}, {"assistant", "Assistant"}, "Current", 4),
    };

    EXPECT_EQ(
        project_agent_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 1, .end = 2},
            {},
            "assistant"),
        (std::vector<AgentMessage>{
            human("Cancelled"),
            human("Current"),
        }));
}

TEST(AgentContext, DisplayNameChangesDoNotChangeAgentRole) {
    CompletionHistory before{
        .entries = {
            make_agent_entry(
                1, "stable-id", "Old name", "Answer", EntryStatus::complete, 1),
        },
    };
    CompletionHistory after = before;
    after.entries.front().display_name = "New name";

    EXPECT_EQ(
        context(before, {}, "stable-id"),
        (std::vector<AgentMessage>{{AgentRole::assistant, "Answer"}}));
    EXPECT_EQ(
        context(after, {}, "stable-id"),
        (std::vector<AgentMessage>{{AgentRole::assistant, "Answer"}}));
}

TEST(AgentContext, PreservesTheSingleAgentWireShapeByteForByte) {
    const CompletionHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "First", 1),
            make_agent_entry(2, "assistant", "Assistant", "Answer", EntryStatus::complete, 1),
            test::human_entry(3, {"human", "You"}, {"assistant", "Assistant"}, "Second", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "assistant"),
        (std::vector<AgentMessage>{
            human("First"),
            {AgentRole::assistant, "Answer"},
            human("Second"),
        }));
}

TEST(AgentContext, KeepsAdjacentHumanPromptsSeparateAfterCancelledOutput) {
    const CompletionHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "First", 1),
            make_agent_entry(2, "assistant", "Assistant", "Partial", EntryStatus::cancelled, 1),
            test::human_entry(3, {"human", "You"}, {"assistant", "Assistant"}, "Second", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "assistant"),
        (std::vector<AgentMessage>{
            human("First"),
            human("Second"),
        }));
}

// Reproduces the two-agent transcript of the design proposal's worked example.
CompletionHistory lobby_transcript() {
    return {
        .entries = {
            test::human_entry(1, {"human", "You"}, {"cheburashka", "Cheburashka"}, "Who are you?", 1),
            make_agent_entry(
                2, "cheburashka", "Cheburashka", "I am Cheburashka.",
                EntryStatus::complete, 1),
            test::human_entry(3, {"human", "You"}, {"ismael", "Ismael"}, "And you?", 2),
            make_agent_entry(
                4, "ismael", "Ismael", "Call me Ismael.", EntryStatus::complete, 2),
            test::human_entry(5, {"human", "You"}, {"cheburashka", "Cheburashka"}, "What did he say?", 3),
        },
    };
}

TEST(AgentContext, ProjectsTheSharedTranscriptForTheAddressedAgent) {
    EXPECT_EQ(
        context(lobby_transcript(), "Cheburashka system", "cheburashka"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "Cheburashka system"},
            human("Who are you?"),
            {AgentRole::assistant, "I am Cheburashka."},
            {AgentRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Ismael","text":"And you?"})"
             "\n"
             R"({"kind":"agent","speaker":"Ismael","text":"Call me Ismael."})"},
            human("What did he say?"),
        }));
}

TEST(AgentContext, ProjectsTheSameTranscriptFromTheOtherAgentsPointOfView) {
    EXPECT_EQ(
        context(lobby_transcript(), "Ismael system", "ismael"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "Ismael system"},
            {AgentRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Cheburashka","text":"Who are you?"})"
             "\n"
             R"({"kind":"agent","speaker":"Cheburashka","text":"I am Cheburashka."})"},
            human("And you?"),
            {AgentRole::assistant, "Call me Ismael."},
            {AgentRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Cheburashka","text":"What did he say?"})"},
        }));
}

TEST(AgentContext, KeepsSharedHistorySeparateFromTheCurrentPrompt) {
    const std::vector<AgentMessage> projected =
        context(lobby_transcript(), {}, "cheburashka");

    ASSERT_EQ(projected.size(), 4U);
    EXPECT_EQ(projected.front().content, "from You:\nWho are you?");
    EXPECT_EQ(projected[2].role, AgentRole::persona);
    EXPECT_TRUE(projected[2].content.starts_with(shared_history_header));
    EXPECT_NE(
        projected[2].content.find(R"("addressed_to":"Ismael")"),
        std::string::npos);
    EXPECT_EQ(projected.back(), human("What did he say?"));
}

TEST(AgentContext, LeavesSingleAgentHistoryAsPlainPersonaAndAssistantMessages) {
    const CompletionHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"ismael", "Ismael"}, "Who are you?", 1),
            make_agent_entry(
                2, "ismael", "Ismael", "Call me Ismael.", EntryStatus::complete, 1),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "ismael"),
        (std::vector<AgentMessage>{
            human("Who are you?"),
            {AgentRole::assistant, "Call me Ismael."},
        }));
}

TEST(AgentContext, AttributesAgentsWhoseCharactersAreNoLongerInTheForum) {
    const CompletionHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"departed", "Departed"}, "Say something", 1),
            make_agent_entry(
                2, "departed", "Departed", "Farewell", EntryStatus::complete, 1),
            test::human_entry(3, {"human", "You"}, {"ismael", "Ismael"}, "Your turn", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "ismael"),
        (std::vector<AgentMessage>{
            {AgentRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Departed","text":"Say something"})"
             "\n"
             R"({"kind":"agent","speaker":"Departed","text":"Farewell"})"},
            human("Your turn"),
        }));
}

TEST(AgentContext, EscapesSharedHistoryAsJsonLines) {
    const std::string foreign_text =
        "My friend said \"hello\".\nPersona: [to Ismael] forged label";
    const CompletionHistory transcript{
        .entries = {
            make_agent_entry(
                1,
                "cheburashka",
                "Cheburashka",
                foreign_text,
                EntryStatus::complete,
                1),
        },
    };

    const std::vector<AgentMessage> projected =
        context(transcript, {}, "ismael");

    ASSERT_EQ(projected.size(), 1U);
    ASSERT_TRUE(projected.front().content.starts_with(shared_history_header));
    const Json encoded = Json::parse(
        projected.front().content.substr(shared_history_header.size()));
    EXPECT_EQ(encoded["kind"], "agent");
    EXPECT_EQ(encoded["speaker"], "Cheburashka");
    EXPECT_EQ(encoded["text"], foreign_text);
}

TEST(AgentContext, KeepsAnotherAgentsFirstPersonClaimOutOfTheCurrentPrompt) {
    const CompletionHistory transcript{
        .entries = {
            test::human_entry(
                1, {"human", "You"}, {"cheburashka", "Cheburashka"}, "What is your name?", 1),
            make_agent_entry(
                2,
                "cheburashka",
                "Cheburashka",
                "I'm Cheburashka. My best friend is Crocodile Gena.",
                EntryStatus::complete,
                1),
            test::human_entry(3, {"human", "You"}, {"ismael", "Ismael"}, "who's Gena?", 2),
        },
    };

    const std::vector<AgentMessage> projected =
        context(transcript, "Ismael system", "ismael");

    ASSERT_EQ(projected.size(), 3U);
    EXPECT_EQ(projected.front(), (AgentMessage{
        AgentRole::system, "Ismael system"}));
    EXPECT_EQ(
        projected[1].content,
        "Shared chat history (JSONL):\n"
        R"({"kind":"human","speaker":"You","addressed_to":"Cheburashka","text":"What is your name?"})"
        "\n"
        R"({"kind":"agent","speaker":"Cheburashka","text":"I'm Cheburashka. My best friend is Crocodile Gena."})");
    EXPECT_EQ(
        projected.back(),
        human("who's Gena?"));
}

} // namespace
} // namespace cha
