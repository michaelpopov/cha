#include "conversation.h"
#include "conversation_file.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <type_traits>

namespace cha {
namespace {

std::filesystem::path temporary_path(std::string_view prefix) {
    return std::filesystem::temp_directory_path()
        / (std::string(prefix)
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + ".data");
}

TEST(Conversation, StoresTypedCompleteAndStreamingEntries) {
    Conversation conversation;
    conversation.add_entry(make_human_entry(1, "Review this code", 10));
    conversation.begin_entry(make_agent_entry(
        2, "reviewer-id", "Reviewer", {}, CompletionStatus::streaming, 10));
    conversation.append_to_entry(2, "Two ");
    conversation.append_to_entry(2, "issues found");

    EXPECT_EQ(conversation.open_entry_id(), 2U);
    conversation.finish_entry(2, CompletionStatus::complete);
    EXPECT_FALSE(conversation.open_entry_id());
    EXPECT_EQ(
        conversation.entries(),
        (std::vector<ConversationEntry>{
            make_human_entry(1, "Review this code", 10),
            make_agent_entry(
                2, "reviewer-id", "Reviewer", "Two issues found", CompletionStatus::complete, 10),
        }));
}

TEST(Conversation, RequiresTheStreamingEntryHandleForMutation) {
    Conversation conversation;
    conversation.begin_entry(make_agent_entry(
        4, "reviewer-id", "Reviewer", {}, CompletionStatus::streaming, 2));

    EXPECT_THROW(conversation.append_to_entry(5, "wrong"), std::logic_error);
    EXPECT_THROW(conversation.discard_entry(5), std::logic_error);
    EXPECT_EQ(conversation.open_entry_id(), 4U);
}

TEST(Conversation, ReturnsAnIndependentEntrySnapshot) {
    Conversation conversation;
    conversation.add_entry(make_notice_entry(1, "Original"));

    auto snapshot = conversation.entries();
    snapshot.front().text = "Changed";

    EXPECT_EQ(conversation.entries().front().text, "Original");
}

TEST(Conversation, ReadViewIsLockedNonOwningAndNeitherCopyableNorMovable) {
    static_assert(!std::is_copy_constructible_v<ConversationReadView>);
    static_assert(!std::is_move_constructible_v<ConversationReadView>);

    Conversation conversation;
    conversation.add_entry(make_notice_entry(1, "Original"));
    const std::size_t revision = conversation.revision();
    ConversationReadView view = conversation.read();

    EXPECT_EQ(view.entries().size(), 1U);
    EXPECT_EQ(view.entries().front().text, "Original");
    EXPECT_EQ(view.revision(), revision);
    EXPECT_FALSE(view.open_entry_id());
}

TEST(Conversation, MutationWaitsUntilTheReadViewIsDestroyed) {
    Conversation conversation;
    conversation.add_entry(make_notice_entry(1, "Original"));
    std::promise<void> view_ready;
    std::future<void> ready = view_ready.get_future();
    std::promise<void> release_view;
    std::shared_future<void> release = release_view.get_future().share();

    std::thread reader([&] {
        ConversationReadView view = conversation.read();
        view_ready.set_value();
        release.wait();
    });
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    std::future<void> mutation = std::async(std::launch::async, [&] {
        conversation.add_entry(make_notice_entry(2, "Later"));
    });
    EXPECT_EQ(mutation.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);

    release_view.set_value();
    reader.join();
    EXPECT_EQ(mutation.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    mutation.get();
}

TEST(Conversation, ReplacesAndClearsEntries) {
    Conversation conversation;
    conversation.add_entry(make_notice_entry(1, "Old"));
    const std::size_t initial_epoch = conversation.snapshot().history_epoch;
    conversation.replace_entries({
        make_human_entry(2, "Restored"),
        make_agent_entry(3, "guide-id", "Guide", "Welcome", CompletionStatus::complete),
    });

    EXPECT_EQ(conversation.entries().size(), 2U);
    EXPECT_EQ(conversation.snapshot().history_epoch, initial_epoch + 1);
    conversation.clear();
    EXPECT_TRUE(conversation.entries().empty());
    EXPECT_EQ(conversation.snapshot().history_epoch, initial_epoch + 2);
}

TEST(Conversation, RequiresStrictlyIncreasingEntryIds) {
    Conversation conversation;
    conversation.add_entry(make_notice_entry(2, "First"));

    EXPECT_THROW(conversation.add_entry(make_notice_entry(2, "Duplicate")), std::invalid_argument);
    EXPECT_THROW(conversation.add_entry(make_notice_entry(1, "Out of order")), std::invalid_argument);
    EXPECT_NO_THROW(conversation.add_entry(make_notice_entry(5, "Gap is allowed")));

    EXPECT_THROW(
        conversation.replace_entries({
            make_notice_entry(10, "Later"),
            make_notice_entry(9, "Earlier"),
        }),
        std::invalid_argument);
}

TEST(ConversationValidation, IsSharedByMemoryAndPersistence) {
    ConversationEntry invalid = make_error_entry(1, "Failure");
    invalid.status = CompletionStatus::complete;
    EXPECT_THROW(validate_conversation_entry(invalid), std::invalid_argument);

    Conversation conversation;
    EXPECT_THROW(conversation.add_entry(invalid), std::invalid_argument);

    const auto path = temporary_path("cha_invalid_entry_");
    ConversationJournal journal(path);
    EXPECT_THROW(journal.append(invalid), std::invalid_argument);

    const ConversationEntry empty_completion = make_agent_entry(
        2, "reviewer-id", "Reviewer", {}, CompletionStatus::complete, 1);
    EXPECT_THROW(validate_conversation_entry(empty_completion), std::invalid_argument);
    EXPECT_THROW(conversation.add_entry(empty_completion), std::invalid_argument);
    EXPECT_THROW(journal.append(empty_completion), std::invalid_argument);

    Conversation streaming;
    streaming.begin_entry(make_agent_entry(
        1, "reviewer-id", "Reviewer", {}, CompletionStatus::streaming, 1));
    EXPECT_THROW(
        streaming.finish_entry(1, CompletionStatus::complete),
        std::invalid_argument);
    std::filesystem::remove(path);
}

TEST(ConversationFile, RoundTripsTypedEntries) {
    const auto path = temporary_path("cha_conversation_");
    Conversation conversation;
    conversation.add_entry(make_human_entry(1, "Hello", 1));
    conversation.add_entry(make_agent_entry(
        2, "reviewer-id", "Reviewer", "Hello back", CompletionStatus::complete, 1));
    conversation.add_entry(make_notice_entry(3, "Information"));
    save_conversation_file(path, conversation);

    EXPECT_EQ(load_conversation_file(path), conversation.entries());
    std::filesystem::remove(path);
}

TEST(ConversationFile, RejectsAStreamingEntry) {
    const auto path = temporary_path("cha_open_conversation_");
    Conversation conversation;
    conversation.begin_entry(make_agent_entry(
        1, "reviewer-id", "Reviewer", {}, CompletionStatus::streaming, 1));

    EXPECT_THROW(save_conversation_file(path, conversation), std::logic_error);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ConversationJournal, ReplaysStandaloneEntriesAndClearEvents) {
    const auto path = temporary_path("cha_journal_");
    ConversationJournal journal(path);
    journal.append(make_notice_entry(1, "Old"));
    journal.clear();
    journal.append(make_notice_entry(2, "Current"));

    EXPECT_EQ(
        load_conversation_file(path),
        (std::vector<ConversationEntry>{make_notice_entry(2, "Current")}));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RejectsOutOfOrderEntryIdsDuringRestore) {
    const auto path = temporary_path("cha_out_of_order_journal_");
    ConversationJournal journal(path);
    journal.append(make_notice_entry(2, "Later ID"));
    journal.append(make_notice_entry(1, "Earlier ID"));

    EXPECT_THROW((void)load_conversation_state(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RepairsAnIncompleteFinalRecord) {
    const auto path = temporary_path("cha_torn_journal_");
    {
        ConversationJournal journal(path);
        journal.append(make_notice_entry(1, "Complete"));
    }
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << R"({"type":"entry","entry":)";
    }

    prepare_conversation_file(path);
    EXPECT_EQ(
        load_conversation_file(path),
        (std::vector<ConversationEntry>{make_notice_entry(1, "Complete")}));

    ConversationJournal journal(path);
    journal.append(make_notice_entry(2, "Recovered"));
    EXPECT_EQ(load_conversation_file(path).size(), 2U);
    std::filesystem::remove(path);
}

TEST(ConversationJournal, ReplaysIdentifiedTypedTurnOutcomes) {
    const auto path = temporary_path("cha_identified_journal_");
    ConversationJournal journal(path);
    journal.start_turn(7, "guide-id", make_human_entry(1, "First", 7));
    journal.complete_turn(7, make_agent_entry(
        2, "guide-id", "Guide", "Answer", CompletionStatus::complete, 7));
    journal.start_turn(8, "guide-id", make_human_entry(3, "Second", 8));
    journal.fail_turn(8, make_error_entry(4, "Unavailable", 8, "guide-id"));

    const ConversationRestore restored = load_conversation_state(path);
    EXPECT_EQ(restored.next_request_id, 9U);
    EXPECT_EQ(restored.next_entry_id, 5U);
    EXPECT_TRUE(restored.interrupted_turns.empty());
    EXPECT_EQ(
        restored.entries,
        (std::vector<ConversationEntry>{
            make_human_entry(1, "First", 7),
            make_agent_entry(2, "guide-id", "Guide", "Answer", CompletionStatus::complete, 7),
            make_human_entry(3, "Second", 8),
            make_error_entry(4, "Unavailable", 8, "guide-id"),
        }));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RejectsEntriesThatDoNotMatchTheirTurnRecords) {
    const auto path = temporary_path("cha_invalid_turn_entry_");
    ConversationJournal journal(path);

    EXPECT_THROW(
        journal.start_turn(7, "guide-id", make_human_entry(1, "Prompt", 8)),
        std::invalid_argument);
    EXPECT_THROW(
        journal.complete_turn(7, make_agent_entry(
            2, "guide-id", "Guide", "Answer", CompletionStatus::cancelled, 7)),
        std::invalid_argument);
    EXPECT_THROW(
        journal.cancel_turn(7, make_agent_entry(
            2, "guide-id", "Guide", "Answer", CompletionStatus::complete, 7)),
        std::invalid_argument);
    EXPECT_THROW(
        journal.fail_turn(7, make_error_entry(2, "Failure", 8, "guide-id")),
        std::invalid_argument);

    std::filesystem::remove(path);
}

TEST(ConversationJournal, RecognizesAnInterruptedTypedTurn) {
    const auto path = temporary_path("cha_interrupted_journal_");
    ConversationJournal journal(path);
    journal.start_turn(12, "guide-id", make_human_entry(5, "Pending", 12));

    const ConversationRestore restored = load_conversation_state(path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    EXPECT_EQ(restored.interrupted_turns.front().request_id, 12U);
    EXPECT_EQ(restored.interrupted_turns.front().error_entry.kind, EntryKind::error);
    EXPECT_EQ(restored.interrupted_turns.front().error_entry.participant_id, "guide-id");
    EXPECT_EQ(restored.next_request_id, 13U);
    EXPECT_EQ(restored.next_entry_id, 7U);
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RejectsThePreviousUntypedFormat) {
    const auto path = temporary_path("cha_old_journal_");
    {
        std::ofstream file(path, std::ios::binary);
        file << R"({"type":"conversation","version":3})" << '\n';
    }

    EXPECT_THROW(ConversationJournal journal(path), std::runtime_error);
    EXPECT_THROW((void)load_conversation_state(path), std::runtime_error);
    std::filesystem::remove(path);
}

} // namespace
} // namespace cha
