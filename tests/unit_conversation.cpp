#include "conversation.h"
#include "conversation_file.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace cha {
namespace {

TEST(Conversation, StoresCompleteAndStreamingMessages) {
    Conversation conversation;
    conversation.add_message("You", "Review this code");
    conversation.begin_message("Reviewer");
    conversation.append_to_message("Two ");
    conversation.append_to_message("issues found");

    EXPECT_TRUE(conversation.message_open());
    EXPECT_EQ(conversation.revision(), 4U);

    conversation.finish_message();
    EXPECT_FALSE(conversation.message_open());
    EXPECT_EQ(
        conversation.messages(),
        (std::vector<ConversationMessage>{
            {"You", "Review this code"},
            {"Reviewer", "Two issues found"},
        }));
}

TEST(Conversation, ReturnsAnIndependentMessageSnapshot) {
    Conversation conversation;
    conversation.add_message("You", "Original");

    auto snapshot = conversation.messages();
    snapshot.front().text = "Changed";

    EXPECT_EQ(conversation.messages().front().text, "Original");
}

TEST(Conversation, ReplacesMessagesForSessionRestore) {
    Conversation conversation;
    conversation.add_message("You", "Old message");
    const std::size_t initial_epoch = conversation.snapshot().history_epoch;
    conversation.replace_messages({{"You", "Restored"}, {"Guide", "Welcome"}});

    EXPECT_EQ(
        conversation.messages(),
        (std::vector<ConversationMessage>{{"You", "Restored"}, {"Guide", "Welcome"}}));
    EXPECT_EQ(conversation.snapshot().history_epoch, initial_epoch + 1);
}

TEST(Conversation, ClearRemovesStoredMessages) {
    Conversation conversation;
    conversation.add_message("You", "Old request");
    conversation.add_message("Guide", "Old response");
    const std::size_t initial_epoch = conversation.snapshot().history_epoch;

    conversation.clear();

    EXPECT_TRUE(conversation.messages().empty());
    EXPECT_EQ(conversation.snapshot().history_epoch, initial_epoch + 1);
}

TEST(ConversationFile, RoundTripsASelfContainedGeneration) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_conversation_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");

    Conversation conversation;
    conversation.add_message("You", "Hello");
    conversation.add_message("Reviewer", "Hello back");
    save_conversation_file(path, conversation);

    EXPECT_EQ(load_conversation_file(path), conversation.messages());

    std::filesystem::remove(path);
}

TEST(ConversationFile, RejectsAnInProgressMessage) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_open_conversation_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");

    Conversation conversation;
    conversation.begin_message("Reviewer");

    EXPECT_THROW(save_conversation_file(path, conversation), std::logic_error);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ConversationJournal, ReplaysMessagesAndClearEvents) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_journal_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".data");

    ConversationJournal journal(path);
    journal.append({"You", "Old request"});
    journal.append({"Guide", "Old response"});
    journal.clear();
    journal.append({"You", "Current request"});

    EXPECT_EQ(
        load_conversation_file(path),
        (std::vector<ConversationMessage>{{"You", "Current request"}}));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RepairsAnIncompleteFinalRecord) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_torn_journal_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".data");

    {
        ConversationJournal journal(path);
        journal.append({"You", "Complete"});
    }
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << R"({"type":"message","author":"Guide")";
    }

    prepare_conversation_file(path);
    EXPECT_EQ(
        load_conversation_file(path),
        (std::vector<ConversationMessage>{{"You", "Complete"}}));

    ConversationJournal journal(path);
    journal.append({"Guide", "Recovered"});
    EXPECT_EQ(
        load_conversation_file(path),
        (std::vector<ConversationMessage>{{"You", "Complete"}, {"Guide", "Recovered"}}));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, ReplaysIdentifiedTurnOutcomes) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_identified_journal_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".data");

    ConversationJournal journal(path);
    journal.start_turn(7, "guide", "First");
    journal.complete_turn(7, "Guide", "Answer");
    journal.start_turn(8, "guide", "Second");
    journal.fail_turn(8, "Unavailable");

    const ConversationRestore restored = load_conversation_state(path);
    EXPECT_EQ(restored.next_request_id, 9U);
    EXPECT_TRUE(restored.interrupted_turns.empty());
    EXPECT_EQ(
        restored.messages,
        (std::vector<ConversationMessage>{
            {"You", "First"},
            {"Guide", "Answer"},
            {"You", "Second"},
            {"System", "Error: Unavailable"},
        }));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RecognizesAnInterruptedIdentifiedTurn) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_interrupted_journal_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".data");

    ConversationJournal journal(path);
    journal.start_turn(12, "guide", "Pending");

    const ConversationRestore restored = load_conversation_state(path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    EXPECT_EQ(restored.interrupted_turns.front().request_id, 12U);
    EXPECT_EQ(restored.next_request_id, 13U);
    EXPECT_EQ(
        restored.messages,
        (std::vector<ConversationMessage>{
            {"You", "Pending"},
            {"System", "Error: Response interrupted before completion"},
        }));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RejectsAnUnsupportedOlderHeader) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_old_journal_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".data");
    {
        std::ofstream file(path, std::ios::binary);
        file << R"({"type":"conversation","version":2})" << '\n'
             << R"({"type":"message","author":"You","text":"Existing"})" << '\n';
    }

    EXPECT_THROW(ConversationJournal journal(path), std::runtime_error);
    EXPECT_THROW((void)load_conversation_state(path), std::runtime_error);
    std::filesystem::remove(path);
}

} // namespace
} // namespace cha
