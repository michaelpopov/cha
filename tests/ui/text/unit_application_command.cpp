#include "ui/text/application_command.h"
#include <gtest/gtest.h>
namespace cha { namespace {
TEST(ApplicationCommand, ParsesQuotedPublicNames) {
    const auto parsed = parse_application_command("/open \"The Forum\" \"Morning Discussion\"");
    ASSERT_TRUE(parsed);
    const auto* command = std::get_if<ApplicationCommand>(&*parsed);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->kind, ApplicationCommandKind::open);
    EXPECT_EQ(command->names, (std::vector<std::string>{"The Forum", "Morning Discussion"}));
}
TEST(ApplicationCommand, SupportsOnlyDocumentedEscapes) {
    const auto parsed = parse_application_command("/iam \"A\\\\B\\\"C\"");
    ASSERT_TRUE(parsed);
    const auto* command = std::get_if<ApplicationCommand>(&*parsed);
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->names[0], "A\\B\"C");
    const auto invalid = parse_application_command("/iam \"A\\nB\"");
    ASSERT_TRUE(invalid);
    EXPECT_EQ(std::get<ApplicationCommandParseError>(*invalid), ApplicationCommandParseError::unsupported_escape);
}
TEST(ApplicationCommand, RejectsArityAndQuoteErrors) {
    EXPECT_EQ(std::get<ApplicationCommandParseError>(*parse_application_command("/forums extra")), ApplicationCommandParseError::extra_argument);
    EXPECT_EQ(std::get<ApplicationCommandParseError>(*parse_application_command("/open Entrance")), ApplicationCommandParseError::missing_argument);
    EXPECT_EQ(std::get<ApplicationCommandParseError>(*parse_application_command("/iam \"")), ApplicationCommandParseError::unmatched_quote);
    EXPECT_EQ(std::get<ApplicationCommandParseError>(*parse_application_command("/iam \"\"")), ApplicationCommandParseError::empty_name);
}
TEST(ApplicationCommand, ParsesEveryCommandAndIgnoresTrailingWhitespace) {
    const std::vector<std::pair<std::string, ApplicationCommandKind>> inputs{
        {"/iam Guest  ", ApplicationCommandKind::iam},
        {"/open Entrance Welcome\t", ApplicationCommandKind::open},
        {"/create Forum Session", ApplicationCommandKind::create},
        {"/forums\n", ApplicationCommandKind::forums},
        {"/sessions Entrance", ApplicationCommandKind::sessions},
        {"/personas", ApplicationCommandKind::personas},
        {"/help", ApplicationCommandKind::help},
    };
    for (const auto& [input, expected] : inputs) {
        const auto parsed = parse_application_command(input);
        ASSERT_TRUE(parsed);
        const auto* command = std::get_if<ApplicationCommand>(&*parsed);
        ASSERT_NE(command, nullptr);
        EXPECT_EQ(command->kind, expected);
    }
}
TEST(ApplicationCommand, LeavesOtherSlashCommandsForTheSessionDispatcher) {
    EXPECT_FALSE(parse_application_command("/mcast Guide hello"));
    EXPECT_FALSE(parse_application_command("/not-a-command"));
    EXPECT_FALSE(parse_application_command("ordinary text"));
}
TEST(ApplicationCommand, ExposesOneCompleteRuntimeCommandCatalog) {
    EXPECT_EQ(command_descriptors().size(), 17U);
    EXPECT_EQ(command_names(),
        "/iam, /open, /create, /forums, /sessions, /personas, /help, /clear, /hide-on, /hide, /hide-off, /mcast, /info, /agents, /@Name, /stop, /exit");
    for (const CommandDescriptor& descriptor : command_descriptors()) {
        EXPECT_FALSE(descriptor.name.empty());
        EXPECT_FALSE(descriptor.syntax.empty());
        EXPECT_FALSE(descriptor.description.empty());
    }
}
} }
