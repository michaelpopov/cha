#include "ui/console/console_startup.h"

#include <gtest/gtest.h>

#include <variant>
#include <vector>

namespace cha {
namespace {

std::variant<ConsoleOptions, ArgumentError> parse(
    std::initializer_list<const char*> arguments) {
    std::vector<const char*> argv(arguments);
    return parse_console_arguments(static_cast<int>(argv.size()), argv.data());
}

TEST(ConsoleStartup, AcceptsOnlyColorAndWholeWorkspaceCheck) {
    const auto ordinary = parse({"chacon", "--color=always"});
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(ordinary));
    EXPECT_EQ(std::get<ConsoleOptions>(ordinary).color, ColorMode::always);
    EXPECT_FALSE(std::get<ConsoleOptions>(ordinary).check);

    const auto checked = parse({"chacon", "--check"});
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(checked));
    EXPECT_TRUE(std::get<ConsoleOptions>(checked).check);
}

TEST(ConsoleStartup, RejectsRemovedAndInvalidOptions) {
    for (const auto& parsed : {
             parse({"chacon", "--persona", "reader"}),
             parse({"chacon", "--forum", "hall"}),
             parse({"chacon", "--session", "saved"}),
             parse({"chacon", "--new", "saved"}),
             parse({"chacon", "--list-forums"}),
             parse({"chacon", "--list-sessions"}),
             parse({"chacon", "--color=sometimes"}),
             parse({"chacon", "operand"}),
         }) {
        ASSERT_TRUE(std::holds_alternative<ArgumentError>(parsed));
        EXPECT_EQ(std::get<ArgumentError>(parsed).exit_code, 2);
    }
}

} // namespace
} // namespace cha
