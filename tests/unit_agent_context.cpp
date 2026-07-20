#include "agent_context.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(AgentContext, ProjectsMessagesForOneNamedAgent) {
    const ConversationSnapshot conversation{
        .messages = {
            {"You", "Draft an answer"},
            {"Writer", "Initial draft"},
            {"You", "Review it"},
            {"Reviewer", "Looks good"},
        },
    };

    EXPECT_EQ(
        build_agent_context(conversation, "Be concise.", "Reviewer"),
        (std::vector<AgentMessage>{
            {"system", "Be concise."},
            {"user", "Draft an answer"},
            {"user", "Writer: Initial draft"},
            {"user", "Review it"},
            {"assistant", "Looks good"},
        }));
}

TEST(AgentContext, OmitsErrorsAndOpenResponses) {
    const ConversationSnapshot conversation{
        .messages = {
            {"You", "Failed request"},
            {"System", "Error: unavailable"},
            {"You", "Current request"},
            {"Reviewer", "Partial response"},
        },
        .message_open = true,
    };

    EXPECT_EQ(
        build_agent_context(conversation, {}, "Reviewer"),
        (std::vector<AgentMessage>{{"user", "Current request"}}));
}

} // namespace
} // namespace cha
