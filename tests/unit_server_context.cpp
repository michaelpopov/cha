#include "server_context.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(ServerContext, ProjectsMessagesForOneNamedServer) {
    const ConversationSnapshot conversation{
        .messages = {
            {"You", "Draft an answer"},
            {"Writer", "Initial draft"},
            {"You", "Review it"},
            {"Reviewer", "Looks good"},
        },
    };

    EXPECT_EQ(
        build_server_context(conversation, "Be concise.", "Reviewer"),
        (std::vector<ServerMessage>{
            {"system", "Be concise."},
            {"user", "Draft an answer"},
            {"user", "Writer: Initial draft"},
            {"user", "Review it"},
            {"assistant", "Looks good"},
        }));
}

TEST(ServerContext, OmitsCommandsErrorsAndOpenResponses) {
    const ConversationSnapshot conversation{
        .messages = {
            {"You", "Discarded by clear"},
            {"Reviewer", "Old response"},
            {"You", ".clear"},
            {"Reviewer", "Conversation cleared."},
            {"You", "Failed request"},
            {"System", "Error: unavailable"},
            {"You", "Current request"},
            {"Reviewer", "Partial response"},
        },
        .message_open = true,
    };

    EXPECT_EQ(
        build_server_context(conversation, {}, "Reviewer"),
        (std::vector<ServerMessage>{{"user", "Current request"}}));
}

} // namespace
} // namespace cha
