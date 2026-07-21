#include "agent_context.h"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

struct MaterializedMessage {
    AgentRole role{};
    std::string content;

    bool operator==(const MaterializedMessage&) const = default;
};

class MaterializingWriter final : public AgentContextWriter {
public:
    void begin_message(AgentRole role) override {
        if (open_) {
            throw std::logic_error("message already open");
        }
        current_ = {.role = role};
        open_ = true;
    }

    void append_content(std::string_view text) override {
        if (!open_) {
            throw std::logic_error("no open message");
        }
        current_.content += text;
    }

    void end_message() override {
        if (!open_) {
            throw std::logic_error("no open message");
        }
        messages.push_back(std::move(current_));
        open_ = false;
    }

    std::vector<MaterializedMessage> messages;

private:
    MaterializedMessage current_;
    bool open_{};
};

std::vector<MaterializedMessage> context(
    const ConversationSnapshot& conversation,
    std::string_view system_prompt,
    std::string_view agent_id) {
    MaterializingWriter writer;
    write_agent_context(
        conversation.entries,
        conversation.open_entry_id,
        system_prompt,
        agent_id,
        writer);
    return writer.messages;
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
        (std::vector<MaterializedMessage>{
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
        (std::vector<MaterializedMessage>{{AgentRole::user, "Current request"}}));
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
        (std::vector<MaterializedMessage>{{AgentRole::assistant, "Answer"}}));
    EXPECT_EQ(
        context(after, {}, "stable-id"),
        (std::vector<MaterializedMessage>{{AgentRole::assistant, "Answer"}}));
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
        (std::vector<MaterializedMessage>{
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
        (std::vector<MaterializedMessage>{
            {AgentRole::user, "First"},
            {AgentRole::user, "Second"},
        }));
}

TEST(AgentContext, SupportsArbitrarilyFragmentedWriterContent) {
    MaterializingWriter writer;
    writer.begin_message(AgentRole::assistant);
    writer.append_content("one");
    writer.append_content(" two");
    writer.append_content(" three");
    writer.append_content(" four");
    writer.end_message();

    EXPECT_EQ(
        writer.messages,
        (std::vector<MaterializedMessage>{{AgentRole::assistant, "one two three four"}}));
}

} // namespace
} // namespace cha
