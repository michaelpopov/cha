#include "agent_protocol.h"

#include <gtest/gtest.h>

#include <string>

namespace cha {
namespace {

CompletionRequest request(std::size_t revision = 1) {
    return {
        .request_id = 7,
        .conversation_revision = revision,
        .prompt = make_human_entry(1, "assistant", "Assistant", "Question", 7),
    };
}

TEST(AgentProtocol, RejectsInvalidStandaloneRequests) {
    CompletionRequest invalid_revision = request(0);
    EXPECT_THROW(validate_completion_request(invalid_revision, "assistant"), std::invalid_argument);

    CompletionRequest invalid_target = request();
    invalid_target.prompt.addressed_to = "other";
    EXPECT_THROW(validate_completion_request(invalid_target, "assistant"), std::invalid_argument);

    CompletionRequest invalid_participant = request();
    invalid_participant.prompt.participant_id = "other";
    EXPECT_THROW(validate_completion_request(invalid_participant, "assistant"), std::invalid_argument);
}

TEST(AgentProtocol, ValidatesMatchingConversationContext) {
    Conversation conversation;
    CompletionRequest completion_request = request();
    conversation.add_entry(completion_request.prompt);
    completion_request.conversation_revision = conversation.revision();
    ConversationReadView view = conversation.read();

    EXPECT_NO_THROW(validate_completion_context(completion_request, view));
}

TEST(AgentProtocol, RejectsRevisionMismatch) {
    Conversation conversation;
    CompletionRequest completion_request = request();
    conversation.add_entry(completion_request.prompt);
    completion_request.conversation_revision = conversation.revision();
    conversation.clear();
    ConversationReadView view = conversation.read();

    EXPECT_THROW(validate_completion_context(completion_request, view), std::invalid_argument);
}

TEST(AgentProtocol, RejectsAnOpenResponseEntry) {
    Conversation conversation;
    CompletionRequest completion_request = request();
    conversation.add_entry(completion_request.prompt);
    conversation.begin_entry(make_agent_entry(
        2, "assistant", "Assistant", {}, CompletionStatus::streaming, 7));
    completion_request.conversation_revision = conversation.revision();
    ConversationReadView view = conversation.read();

    EXPECT_THROW(validate_completion_context(completion_request, view), std::invalid_argument);
}

TEST(AgentProtocol, RejectsEmptyConversation) {
    Conversation conversation;
    CompletionRequest completion_request = request();
    ConversationReadView view = conversation.read();

    EXPECT_THROW(validate_completion_context(completion_request, view), std::invalid_argument);
}

TEST(AgentProtocol, RejectsPromptThatIsNotLatestEntry) {
    Conversation conversation;
    CompletionRequest completion_request = request();
    conversation.add_entry(completion_request.prompt);
    conversation.add_entry(make_notice_entry(2, "Later"));
    completion_request.conversation_revision = conversation.revision();
    ConversationReadView view = conversation.read();

    EXPECT_THROW(validate_completion_context(completion_request, view), std::invalid_argument);
}

} // namespace
} // namespace cha
