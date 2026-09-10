#include "web/text_command.h"

#include <gtest/gtest.h>

namespace cha::web {
namespace {

TEST(Command, TreatsOrdinaryInputAsText) {
    EXPECT_EQ(parse_command("").kind, CommandKind::text);
    EXPECT_EQ(parse_command("hello").kind, CommandKind::text);
    EXPECT_EQ(parse_command("@Ismael /clear").kind, CommandKind::text);
    EXPECT_EQ(parse_command(" /clear").kind, CommandKind::text)
        << "commands are not trimmed before recognition";
}

TEST(Command, RecognizesOnlyConversationContextCommands) {
    EXPECT_EQ(parse_command("/mcast").kind, CommandKind::mcast);
    EXPECT_EQ(parse_command("/cover").kind, CommandKind::cover);
    EXPECT_EQ(parse_command("/uncover").kind, CommandKind::uncover);
    EXPECT_EQ(parse_command("/clear").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/info").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/characters").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/agents").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/@Guide").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/style").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/stop").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/exit").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/nonsense").kind, CommandKind::unknown);
}

TEST(Command, CapturesMulticastText) {
    const Command multicast = parse_command("/mcast @One, @Two. Question");
    EXPECT_EQ(multicast.kind, CommandKind::mcast);
    EXPECT_EQ(multicast.argument, "@One, @Two. Question");
}

TEST(Command, ListsOnlyCommandsAcceptedByTheWebRawInputPath) {
    EXPECT_EQ(command_names(), "/cover, /uncover, /mcast");
}

} // namespace
} // namespace cha::web
