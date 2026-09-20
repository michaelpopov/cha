#include "web/text_command.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(Command, ParsesOnlySupportedCommands) {
    struct Case {
        const char* input;
        CommandKind kind;
    };
    const Case cases[]{
        {"", CommandKind::text},
        {"hello", CommandKind::text},
        {"@Ismael /clear", CommandKind::text},
        {" /clear", CommandKind::text},
        {"/mcast", CommandKind::mcast},
        {"/cover", CommandKind::unknown},
        {"/uncover", CommandKind::unknown},
        {"/clear", CommandKind::unknown},
        {"/info", CommandKind::unknown},
        {"/characters", CommandKind::unknown},
        {"/agents", CommandKind::unknown},
        {"/@Guide", CommandKind::unknown},
        {"/style", CommandKind::unknown},
        {"/stop", CommandKind::unknown},
        {"/exit", CommandKind::unknown},
        {"/nonsense", CommandKind::unknown},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.input);
        EXPECT_EQ(parse_command(item.input).kind, item.kind);
    }
    const Command multicast = parse_command("/mcast @One, @Two. Question");
    EXPECT_EQ(multicast.kind, CommandKind::mcast);
    EXPECT_EQ(multicast.argument, "@One, @Two. Question");
    EXPECT_EQ(command_names(), "/mcast");
}

} // namespace
} // namespace cha
