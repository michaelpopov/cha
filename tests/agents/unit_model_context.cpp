#include "characters/model_context.h"
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

std::vector<ModelMessage> context(
    const ModelHistory& transcript,
    std::string_view system_prompt,
    std::string_view character_id) {
    return project_model_context(
        transcript.entries,
        transcript.open_entry_id,
        {},
        system_prompt,
        character_id);
}

ModelMessage human(std::string_view text) {
    return {ModelRole::persona, "from You:\n" + std::string(text)};
}

TEST(ModelContext, ProjectsRolesFromKindsAndStableParticipantIds) {
    const ModelHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"reviewer-id", "Reviewer"}, "Draft an answer", 1),
            make_character_entry(
                2, "writer-id", "You", "Initial draft", EntryStatus::complete, 1),
            test::human_entry(3, {"human", "You"}, {"reviewer-id", "Reviewer"}, "Review it", 2),
            make_character_entry(
                4, "reviewer-id", "System", "Looks good", EntryStatus::complete, 2),
        },
    };

    EXPECT_EQ(
        context(transcript, "Be concise.", "reviewer-id"),
        (std::vector<ModelMessage>{
            {ModelRole::system, "Be concise."},
            human("Draft an answer"),
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"character","speaker":"You","text":"Initial draft"})"},
            human("Review it"),
            {ModelRole::assistant, "Looks good"},
        }));
}

TEST(ModelContext, OmitsNoticesErrorsFailedPromptsAndIncompleteCharacterEntries) {
    const ModelHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"reviewer-id", "Reviewer"}, "Failed request", 7),
            make_error_entry(2, "unavailable", 7, "reviewer-id"),
            make_notice_entry(3, "Model information"),
            test::human_entry(4, {"human", "You"}, {"reviewer-id", "Reviewer"}, "Current request", 8),
            make_character_entry(
                5, "reviewer-id", "Reviewer", "Stopped", EntryStatus::cancelled, 8),
            make_character_entry(
                6, "reviewer-id", "Reviewer", "Partial", EntryStatus::streaming, 8),
        },
        .open_entry_id = 6,
    };

    EXPECT_EQ(
        context(transcript, {}, "reviewer-id"),
        (std::vector<ModelMessage>{human("Current request")}));
}

TEST(ModelContext, OmitsTheClosedOffrecordSpan) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Before", 1),
        make_hide_on_marker(2),
        test::human_entry(3, {"human", "You"}, {"assistant", "Assistant"}, "Hidden", 2),
        make_character_entry(4, "assistant", "Assistant", "Hidden answer", EntryStatus::complete, 2),
        make_hide_marker(5),
        test::human_entry(6, {"human", "You"}, {"assistant", "Assistant"}, "After", 3),
    };

    EXPECT_EQ(
        project_model_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 2, .end = 5},
            {},
            "assistant"),
        (std::vector<ModelMessage>{
            human("Before"),
            human("After"),
        }));
}

TEST(ModelContext, AnOpenOffrecordSpanExcludesNothing) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Before", 1),
        make_hide_on_marker(2),
        test::human_entry(3, {"human", "You"}, {"assistant", "Assistant"}, "After opening", 2),
    };

    EXPECT_EQ(
        project_model_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 2, .end = std::nullopt},
            {},
            "assistant"),
        (std::vector<ModelMessage>{
            human("Before"),
            human("After opening"),
        }));
}

TEST(ModelContext, ProjectsOnlyEntriesOutsideAClosedOffrecordSpan) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"one", "One"}, "Question", 1),
        make_character_entry(2, "one", "One", "One answer", EntryStatus::complete, 1),
        test::human_entry(3, {"human", "You"}, {"two", "Two"}, "Question", 2),
    };

    EXPECT_EQ(
        project_model_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 1, .end = 3},
            {},
            "two"),
        (std::vector<ModelMessage>{human("Question")}));
}

TEST(ModelContext, ImmutableInputKeepsATrailingSharedBlockSeparateFromPrompt) {
    auto history = std::make_shared<const ModelHistory>(
        ModelHistory{
            .entries = {
                test::human_entry(
                    1, {"human", "You"}, {"other", "Other"}, "Other question", 1),
                make_character_entry(
                    2,
                    "other",
                    "Other",
                    "Other answer",
                    EntryStatus::complete,
                    1),
            },
        });
    const GenerationRequest input{
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

    const std::vector<ModelMessage> projected =
        project_model_context(input, {});

    EXPECT_EQ(
        projected,
        project_model_context(
            old_tail, std::nullopt, {}, {}, "assistant"));
    EXPECT_EQ(
        projected,
        (std::vector<ModelMessage>{
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Other","text":"Other question"})"
             "\n"
             R"({"kind":"character","speaker":"Other","text":"Other answer"})"},
            human("Current question"),
        }));
}

TEST(ModelContext, PrefixesBothReplayedAndLivePromptsWithTheirOwnAuthors) {
    const ModelHistory history{
        .entries = {
            test::human_entry(
                1, {"reader", "Reader"}, {"assistant", "Assistant"},
                "Earlier prompt", 1),
        },
    };
    const GenerationRequest input{
        .history = std::make_shared<const ModelHistory>(history),
        .run = {
            .request_id = 2,
            .target = {"assistant", "Assistant"},
            .author = {"athlete", "Athlete"},
            .prompt_text = "Current prompt",
        },
    };

    EXPECT_EQ(
        project_model_context(input, {}),
        (std::vector<ModelMessage>{
            {ModelRole::persona, "from Reader:\nEarlier prompt"},
            {ModelRole::persona, "from Athlete:\nCurrent prompt"},
        }));
}

TEST(ModelContext, ProjectsUtcTimestampsWhenRunHasSubmissionTime) {
    TranscriptEntry earlier_question = test::human_entry(
        1, {"human", "You"}, {"assistant", "Assistant"}, "Earlier question", 1);
    earlier_question.created_at = 1'700'000'000;
    TranscriptEntry earlier_answer = make_character_entry(
        2, "assistant", "Assistant", "Earlier answer", EntryStatus::complete, 1);
    earlier_answer.created_at = 1'700'000'001;
    TranscriptEntry shared_question = test::human_entry(
        3, {"human", "You"}, {"other", "Other"}, "Other question", 2);
    shared_question.created_at = 1'700'000'002;

    const GenerationRequest input{
        .history = std::make_shared<const ModelHistory>(ModelHistory{
            .entries = {
                std::move(earlier_question),
                std::move(earlier_answer),
                std::move(shared_question),
            },
        }),
        .run = {
            .request_id = 3,
            .target = {"assistant", "Assistant"},
            .author = {"human", "You"},
            .prompt_text = "Current question",
            .created_at = 1'700'000'003,
        },
    };

    EXPECT_EQ(
        project_model_context(input, "System"),
        (std::vector<ModelMessage>{
            {ModelRole::system, "System"},
            {ModelRole::persona,
             "from You at 2023-11-14T22:13:20Z:\nEarlier question"},
            {ModelRole::assistant,
             "[2023-11-14T22:13:21Z]\nEarlier answer"},
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Other","created_at":"2023-11-14T22:13:22Z","text":"Other question"})"},
            {ModelRole::persona,
             "from You at 2023-11-14T22:13:23Z:\nCurrent question"},
        }));
}

TEST(ModelContext, SubmissionTimeChangesOnlyTheFinalMessage) {
    const auto make_input = [](std::int64_t created_at) {
        TranscriptEntry earlier_question = test::human_entry(
            1, {"human", "You"}, {"assistant", "Assistant"}, "Earlier question", 1);
        earlier_question.created_at = 1'700'000'000;
        TranscriptEntry earlier_answer = make_character_entry(
            2, "assistant", "Assistant", "Earlier answer", EntryStatus::complete, 1);
        earlier_answer.created_at = 1'700'000'001;
        return GenerationRequest{
            .history = std::make_shared<const ModelHistory>(ModelHistory{
                .entries = {std::move(earlier_question), std::move(earlier_answer)},
            }),
            .run = {
                .request_id = 1,
                .target = {"assistant", "Assistant"},
                .author = {"human", "You"},
                .prompt_text = "Current question",
                .created_at = created_at,
            },
        };
    };

    const std::vector<ModelMessage> first =
        project_model_context(make_input(1'700'000'003), "System");
    const std::vector<ModelMessage> second =
        project_model_context(make_input(1'700'000'004), "System");

    ASSERT_EQ(first.size(), 4U);
    ASSERT_EQ(second.size(), 4U);
    for (std::size_t index = 0; index + 1 < first.size(); ++index) {
        EXPECT_EQ(first[index], second[index]);
    }
    EXPECT_NE(first.back(), second.back());
    EXPECT_EQ(first.back().content, "from You at 2023-11-14T22:13:23Z:\nCurrent question");
    EXPECT_EQ(second.back().content, "from You at 2023-11-14T22:13:24Z:\nCurrent question");
}

TEST(ModelContext, SplicesHiddenTurnsOutOfOneSharedHistoryBlock) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"other", "Other"}, "First question", 1),
        make_character_entry(2, "other", "Other", "First answer", EntryStatus::complete, 1),
        make_hide_on_marker(3),
        test::human_entry(4, {"human", "You"}, {"assistant", "Assistant"}, "Hidden question", 2),
        make_character_entry(5, "assistant", "Assistant", "Hidden answer", EntryStatus::complete, 2),
        make_hide_marker(6),
        test::human_entry(7, {"human", "You"}, {"other", "Other"}, "Second question", 3),
        make_character_entry(8, "other", "Other", "Second answer", EntryStatus::complete, 3),
        test::human_entry(9, {"human", "You"}, {"assistant", "Assistant"}, "Current", 4),
    };

    EXPECT_EQ(
        project_model_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 3, .end = 6},
            {},
            "assistant"),
        (std::vector<ModelMessage>{
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Other","text":"First question"})"
             "\n"
             R"({"kind":"character","speaker":"Other","text":"First answer"})"
             "\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Other","text":"Second question"})"
             "\n"
             R"({"kind":"character","speaker":"Other","text":"Second answer"})"},
            human("Current"),
        }));
}

TEST(ModelContext, SpanCanHideAllEarlierTurnsWithoutHidingTheCurrentPrompt) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Earlier", 1),
        make_character_entry(2, "assistant", "Assistant", "Earlier answer", EntryStatus::complete, 1),
        make_hide_on_marker(3),
        make_hide_marker(4),
        test::human_entry(5, {"human", "You"}, {"assistant", "Assistant"}, "Current", 2),
    };

    EXPECT_EQ(
        project_model_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 1, .end = 5},
            "System",
            "assistant"),
        (std::vector<ModelMessage>{
            {ModelRole::system, "System"},
            human("Current"),
        }));
}

TEST(ModelContext, CombinesSpanExclusionWithFailedAndCancelledTurnRules) {
    const std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Hidden", 1),
        test::human_entry(2, {"human", "You"}, {"assistant", "Assistant"}, "Failed", 2),
        make_error_entry(3, "unavailable", 2, "assistant"),
        test::human_entry(4, {"human", "You"}, {"assistant", "Assistant"}, "Cancelled", 3),
        make_character_entry(5, "assistant", "Assistant", "Partial", EntryStatus::cancelled, 3),
        test::human_entry(6, {"human", "You"}, {"assistant", "Assistant"}, "Current", 4),
    };

    EXPECT_EQ(
        project_model_context(
            entries,
            std::nullopt,
            OffrecordSpan{.begin = 1, .end = 2},
            {},
            "assistant"),
        (std::vector<ModelMessage>{
            human("Cancelled"),
            human("Current"),
        }));
}

TEST(ModelContext, DisplayNameChangesDoNotChangeModelRole) {
    ModelHistory before{
        .entries = {
            make_character_entry(
                1, "stable-id", "Old name", "Answer", EntryStatus::complete, 1),
        },
    };
    ModelHistory after = before;
    after.entries.front().display_name = "New name";

    EXPECT_EQ(
        context(before, {}, "stable-id"),
        (std::vector<ModelMessage>{{ModelRole::assistant, "Answer"}}));
    EXPECT_EQ(
        context(after, {}, "stable-id"),
        (std::vector<ModelMessage>{{ModelRole::assistant, "Answer"}}));
}

TEST(ModelContext, PreservesTheSingleCharacterWireShapeByteForByte) {
    const ModelHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "First", 1),
            make_character_entry(2, "assistant", "Assistant", "Answer", EntryStatus::complete, 1),
            test::human_entry(3, {"human", "You"}, {"assistant", "Assistant"}, "Second", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "assistant"),
        (std::vector<ModelMessage>{
            human("First"),
            {ModelRole::assistant, "Answer"},
            human("Second"),
        }));
}

TEST(ModelContext, KeepsAdjacentHumanPromptsSeparateAfterCancelledOutput) {
    const ModelHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "First", 1),
            make_character_entry(2, "assistant", "Assistant", "Partial", EntryStatus::cancelled, 1),
            test::human_entry(3, {"human", "You"}, {"assistant", "Assistant"}, "Second", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "assistant"),
        (std::vector<ModelMessage>{
            human("First"),
            human("Second"),
        }));
}

// Reproduces the two-character transcript of the design proposal's worked example.
ModelHistory lobby_transcript() {
    return {
        .entries = {
            test::human_entry(1, {"human", "You"}, {"cheburashka", "Cheburashka"}, "Who are you?", 1),
            make_character_entry(
                2, "cheburashka", "Cheburashka", "I am Cheburashka.",
                EntryStatus::complete, 1),
            test::human_entry(3, {"human", "You"}, {"ismael", "Ismael"}, "And you?", 2),
            make_character_entry(
                4, "ismael", "Ismael", "Call me Ismael.", EntryStatus::complete, 2),
            test::human_entry(5, {"human", "You"}, {"cheburashka", "Cheburashka"}, "What did he say?", 3),
        },
    };
}

TEST(ModelContext, ProjectsNullAgentMonologueAsConsecutiveSharedHistory) {
    const ModelHistory transcript{
        .entries = {
            test::human_entry(1, {"you", "You"}, {"-", "-"}, "First thought"),
            test::human_entry(2, {"you", "You"}, {"-", "-"}, "Second thought"),
        },
    };

    // Messages recorded against the null target are never first-person
    // prompts; they land in one shared-history block for any character.
    EXPECT_EQ(
        context(transcript, "Cheburashka system", "cheburashka"),
        (std::vector<ModelMessage>{
            {ModelRole::system, "Cheburashka system"},
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"-","text":"First thought"})"
             "\n"
             R"({"kind":"human","speaker":"You","addressed_to":"-","text":"Second thought"})"},
        }));
}

TEST(ModelContext, ProjectsTheSharedTranscriptForTheAddressedCharacter) {
    EXPECT_EQ(
        context(lobby_transcript(), "Cheburashka system", "cheburashka"),
        (std::vector<ModelMessage>{
            {ModelRole::system, "Cheburashka system"},
            human("Who are you?"),
            {ModelRole::assistant, "I am Cheburashka."},
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Ismael","text":"And you?"})"
             "\n"
             R"({"kind":"character","speaker":"Ismael","text":"Call me Ismael."})"},
            human("What did he say?"),
        }));
}

TEST(ModelContext, ProjectsTheSameTranscriptFromTheOtherCharactersPointOfView) {
    EXPECT_EQ(
        context(lobby_transcript(), "Ismael system", "ismael"),
        (std::vector<ModelMessage>{
            {ModelRole::system, "Ismael system"},
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Cheburashka","text":"Who are you?"})"
             "\n"
             R"({"kind":"character","speaker":"Cheburashka","text":"I am Cheburashka."})"},
            human("And you?"),
            {ModelRole::assistant, "Call me Ismael."},
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Cheburashka","text":"What did he say?"})"},
        }));
}

TEST(ModelContext, KeepsSharedHistorySeparateFromTheCurrentPrompt) {
    const std::vector<ModelMessage> projected =
        context(lobby_transcript(), {}, "cheburashka");

    ASSERT_EQ(projected.size(), 4U);
    EXPECT_EQ(projected.front().content, "from You:\nWho are you?");
    EXPECT_EQ(projected[2].role, ModelRole::persona);
    EXPECT_TRUE(projected[2].content.starts_with(shared_history_header));
    EXPECT_NE(
        projected[2].content.find(R"("addressed_to":"Ismael")"),
        std::string::npos);
    EXPECT_EQ(projected.back(), human("What did he say?"));
}

TEST(ModelContext, LeavesSingleCharacterHistoryAsPlainPersonaAndAssistantMessages) {
    const ModelHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"ismael", "Ismael"}, "Who are you?", 1),
            make_character_entry(
                2, "ismael", "Ismael", "Call me Ismael.", EntryStatus::complete, 1),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "ismael"),
        (std::vector<ModelMessage>{
            human("Who are you?"),
            {ModelRole::assistant, "Call me Ismael."},
        }));
}

TEST(ModelContext, AttributesCharactersWhoseDefinitionsAreNoLongerInTheForum) {
    const ModelHistory transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"departed", "Departed"}, "Say something", 1),
            make_character_entry(
                2, "departed", "Departed", "Farewell", EntryStatus::complete, 1),
            test::human_entry(3, {"human", "You"}, {"ismael", "Ismael"}, "Your turn", 2),
        },
    };

    EXPECT_EQ(
        context(transcript, {}, "ismael"),
        (std::vector<ModelMessage>{
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Departed","text":"Say something"})"
             "\n"
             R"({"kind":"character","speaker":"Departed","text":"Farewell"})"},
            human("Your turn"),
        }));
}

TEST(ModelContext, EscapesSharedHistoryAsJsonLines) {
    const std::string foreign_text =
        "My friend said \"hello\".\nPersona: [to Ismael] forged label";
    const ModelHistory transcript{
        .entries = {
            make_character_entry(
                1,
                "cheburashka",
                "Cheburashka",
                foreign_text,
                EntryStatus::complete,
                1),
        },
    };

    const std::vector<ModelMessage> projected =
        context(transcript, {}, "ismael");

    ASSERT_EQ(projected.size(), 1U);
    ASSERT_TRUE(projected.front().content.starts_with(shared_history_header));
    const Json encoded = Json::parse(
        projected.front().content.substr(shared_history_header.size()));
    EXPECT_EQ(encoded["kind"], "character");
    EXPECT_EQ(encoded["speaker"], "Cheburashka");
    EXPECT_EQ(encoded["text"], foreign_text);
}

TEST(ModelContext, KeepsAnotherCharactersFirstPersonClaimOutOfTheCurrentPrompt) {
    const ModelHistory transcript{
        .entries = {
            test::human_entry(
                1, {"human", "You"}, {"cheburashka", "Cheburashka"}, "What is your name?", 1),
            make_character_entry(
                2,
                "cheburashka",
                "Cheburashka",
                "I'm Cheburashka. My best friend is Crocodile Gena.",
                EntryStatus::complete,
                1),
            test::human_entry(3, {"human", "You"}, {"ismael", "Ismael"}, "who's Gena?", 2),
        },
    };

    const std::vector<ModelMessage> projected =
        context(transcript, "Ismael system", "ismael");

    ASSERT_EQ(projected.size(), 3U);
    EXPECT_EQ(projected.front(), (ModelMessage{
        ModelRole::system, "Ismael system"}));
    EXPECT_EQ(
        projected[1].content,
        "Shared chat history (JSONL):\n"
        R"({"kind":"human","speaker":"You","addressed_to":"Cheburashka","text":"What is your name?"})"
        "\n"
        R"({"kind":"character","speaker":"Cheburashka","text":"I'm Cheburashka. My best friend is Crocodile Gena."})");
    EXPECT_EQ(
        projected.back(),
        human("who's Gena?"));
}

} // namespace
} // namespace cha
