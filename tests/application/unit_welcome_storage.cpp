#include "application/welcome_storage.h"
#include "session/session_database.h"

#include <gtest/gtest.h>

TEST(WelcomeStorage, IsFreshAndPrivateToItsOwner) {
    cha::WelcomeStorage first;
    cha::WelcomeStorage second;
    EXPECT_NE(first.database_path(), second.database_path());
    EXPECT_TRUE(cha::load_session_state(first.database_path()).entries.empty());
    const auto path = first.directory();
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(WelcomeStorage, RemovesOnlyItsOwnedDirectory) {
    std::filesystem::path path;
    {
        cha::WelcomeStorage storage;
        path = storage.directory();
        ASSERT_TRUE(std::filesystem::exists(path));
    }
    EXPECT_FALSE(std::filesystem::exists(path));
}
