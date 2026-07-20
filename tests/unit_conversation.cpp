#include "conversation.h"
#include "conversation_file.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

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
    conversation.replace_messages({{"You", "Restored"}, {"Guide", "Welcome"}});

    EXPECT_EQ(
        conversation.messages(),
        (std::vector<ConversationMessage>{{"You", "Restored"}, {"Guide", "Welcome"}}));
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

} // namespace
} // namespace cha
