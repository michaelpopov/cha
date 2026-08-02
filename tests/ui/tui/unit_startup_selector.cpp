#include "ui/tui/startup_selector.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(StartupSelector, PresentsUserDisplayNamesInRosterOrder) {
    const UserRoster users{
        {.id = "reader", .display_name = "Reader", .prompt = "Read closely."},
        {.id = "writer", .display_name = "Writer", .prompt = "Write clearly."},
    };

    EXPECT_EQ(
        StartupSelector::user_display_names(users),
        (std::vector<std::string>{"Reader", "Writer"}));
}

TEST(StartupSelector, ReturnsTheSelectedUserOrCancellation) {
    const UserRoster users{
        {.id = "reader", .display_name = "Reader", .prompt = "Read closely."},
        {.id = "writer", .display_name = "Writer", .prompt = "Write clearly."},
    };

    const std::optional<User> selected =
        StartupSelector::selected_user(users, 1);
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->id, "writer");
    EXPECT_EQ(selected->display_name, "Writer");
    EXPECT_EQ(selected->prompt, "Write clearly.");
    EXPECT_FALSE(StartupSelector::selected_user(users, std::nullopt));
}

} // namespace
} // namespace cha
