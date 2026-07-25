#include "ui/text/command.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(Command, TreatsAnythingWithoutALeadingSlashAsText) {
    EXPECT_EQ(parse_command("").kind, CommandKind::text);
    EXPECT_EQ(parse_command("hello").kind, CommandKind::text);
    EXPECT_EQ(parse_command("@Ismael /clear").kind, CommandKind::text);
    EXPECT_EQ(parse_command(" /clear").kind, CommandKind::text)
        << "commands are not trimmed before recognition";
}

TEST(Command, RecognizesEveryArgumentlessCommand) {
    EXPECT_EQ(parse_command("/clear").kind, CommandKind::clear);
    EXPECT_EQ(parse_command("/info").kind, CommandKind::info);
    EXPECT_EQ(parse_command("/stop").kind, CommandKind::stop);
    EXPECT_EQ(parse_command("/exit").kind, CommandKind::exit);
    EXPECT_EQ(parse_command("/agents").kind, CommandKind::agents);
    EXPECT_EQ(parse_command("/nonsense").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/clearly").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/model other-model").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/stop\n").kind, CommandKind::stop);
    EXPECT_EQ(parse_command("/stop \r\n").argument, "");
    EXPECT_TRUE(parse_command("/agents").argument.empty());
    EXPECT_TRUE(parse_command("/agents").handle.empty());
}

TEST(Command, CapturesTrailingTextAsTheRejectableArgument) {
    const Command clear = parse_command("/clear everything");
    EXPECT_EQ(clear.kind, CommandKind::clear);
    EXPECT_EQ(clear.argument, "everything");

    const Command agents = parse_command("/agents   please  ");
    EXPECT_EQ(agents.kind, CommandKind::agents);
    EXPECT_EQ(agents.argument, "please");
}

TEST(Command, ParsesTheDefaultAgentCommandHandle) {
    const Command named = parse_command("/@Ismael");
    EXPECT_EQ(named.kind, CommandKind::set_default);
    EXPECT_EQ(named.handle, "Ismael");
    EXPECT_TRUE(named.argument.empty());

    const Command prefix = parse_command("/@che");
    EXPECT_EQ(prefix.kind, CommandKind::set_default);
    EXPECT_EQ(prefix.handle, "che");

    // Handles are passed through verbatim; RoomPersonas interprets them.
    const Command punctuated = parse_command("/@Ismael,");
    EXPECT_EQ(punctuated.handle, "Ismael,");
}

TEST(Command, ReportsABareSlashAtAsSetDefaultWithoutAHandle) {
    const Command bare = parse_command("/@");
    EXPECT_EQ(bare.kind, CommandKind::set_default);
    EXPECT_TRUE(bare.handle.empty());
    EXPECT_TRUE(bare.argument.empty());

    const Command spaced = parse_command("/@ Ismael");
    EXPECT_EQ(spaced.kind, CommandKind::set_default);
    EXPECT_TRUE(spaced.handle.empty());
    EXPECT_EQ(spaced.argument, "Ismael");
}

TEST(Command, SeparatesTheDefaultAgentHandleFromAnArgument) {
    const Command extra = parse_command("/@Ismael hi");
    EXPECT_EQ(extra.kind, CommandKind::set_default);
    EXPECT_EQ(extra.handle, "Ismael");
    EXPECT_EQ(extra.argument, "hi")
        << "a non-empty argument is what rejects /@Name with trailing words";
}

} // namespace
} // namespace cha
