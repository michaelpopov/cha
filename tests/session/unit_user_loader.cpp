#include "session/workspace.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace cha {
namespace {

class UserLoaderTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_users_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "forums");
        std::ofstream(root_ / "app.toml")
            << "host = \"127.0.0.1\"\n"
               "port = 8080\n"
               "[logging]\n"
               "file = \"cha.log\"\n"
               "level = \"off\"\n";
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    void add_user(
        std::string_view id,
        std::string_view display_name,
        std::string_view prompt = "") {
        const std::filesystem::path directory = root_ / "users" / id;
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "user.toml")
            << "display_name = \"" << display_name << "\"\n";
        if (!prompt.empty()) std::ofstream(directory / "USER.md") << prompt;
    }

    std::filesystem::path root_;
};

TEST_F(UserLoaderTest, LoadsUsersInLexicographicIdOrderAndReadsOptionalPrompt) {
    add_user("zebra", "Zebra", "Verbatim\ntext");
    add_user("alpha", "Alpha");

    const UserRoster users = Workspace(root_).load_users();

    ASSERT_EQ(users.size(), 2U);
    EXPECT_EQ(users[0].id, "alpha");
    EXPECT_EQ(users[0].display_name, "Alpha");
    EXPECT_TRUE(users[0].prompt.empty());
    EXPECT_EQ(users[1].id, "zebra");
    EXPECT_EQ(users[1].display_name, "Zebra");
    EXPECT_EQ(users[1].prompt, "Verbatim\ntext");
}

TEST_F(UserLoaderTest, ReloadsTheRosterOnEveryCall) {
    add_user("ada", "Ada");
    Workspace workspace(root_);

    EXPECT_EQ(workspace.load_users().size(), 1U);

    add_user("bert", "Bert");
    const UserRoster users = workspace.load_users();
    ASSERT_EQ(users.size(), 2U);
    EXPECT_EQ(users[1].id, "bert");
}

TEST_F(UserLoaderTest, RequiresUsersDirectoryAndAtLeastOneUser) {
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    std::filesystem::create_directories(root_ / "users");
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);
}

TEST_F(UserLoaderTest, RequiresAUserTomlForEveryUserDirectory) {
    std::filesystem::create_directories(root_ / "users" / "missing");

    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);
}

TEST_F(UserLoaderTest, RequiresUserPromptToBeARegularFile) {
    add_user("ada", "Ada");
    std::filesystem::create_directory(root_ / "users" / "ada" / "USER.md");

    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);
}

TEST_F(UserLoaderTest, RejectsUnknownOrMissingOrWrongTypedConfigFields) {
    add_user("ada", "Ada");
    {
        std::ofstream config(root_ / "users" / "ada" / "user.toml");
        config << "display_name = \"Ada\"\nextra = true\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    {
        std::ofstream config(root_ / "users" / "ada" / "user.toml");
        config << "other = \"Ada\"\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    {
        std::ofstream config(root_ / "users" / "ada" / "user.toml");
        config << "display_name = 42\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);
}

TEST_F(UserLoaderTest, RejectsInvalidAndReservedIds) {
    add_user("bad-id", "Ada");
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    std::filesystem::remove_all(root_ / "users");
    add_user("9ada", "Ada");
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    std::filesystem::remove_all(root_ / "users");
    add_user("System", "Ada");
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);
}

TEST_F(UserLoaderTest, RejectsInvalidReservedAndDuplicateDisplayNames) {
    add_user("ada", " Ada");
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    {
        std::ofstream config(root_ / "users" / "ada" / "user.toml");
        config << "display_name = \"@Ada\"\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    {
        std::ofstream config(root_ / "users" / "ada" / "user.toml");
        config << "display_name = \"System\"\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    {
        std::ofstream config(root_ / "users" / "ada" / "user.toml");
        config << "display_name = \"Ada\\u0001\"\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    {
        std::ofstream config(root_ / "users" / "ada" / "user.toml");
        config << "display_name = \"Ada\"\n";
    }
    add_user("other", "aDA");
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);
}

TEST_F(UserLoaderTest, RejectsUnicodeControlsLineBreaksAndBoundaryWhitespace) {
    add_user("ada", "Ada");
    const std::filesystem::path config_path =
        root_ / "users" / "ada" / "user.toml";

    std::ofstream(config_path)
        << "display_name = \"Ada\\u0085Lovelace\"\n";
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    std::ofstream(config_path)
        << "display_name = \"Ada\\u2028Lovelace\"\n";
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    std::ofstream(config_path)
        << "display_name = \"\\u00a0Ada\"\n";
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    std::ofstream(config_path)
        << "display_name = \"Ada\\u3000\"\n";
    EXPECT_THROW((void)Workspace(root_).load_users(), std::runtime_error);

    std::ofstream(config_path)
        << "display_name = \"Ren\\u00e9e \\u674e\"\n";
    const UserRoster users = Workspace(root_).load_users();
    ASSERT_EQ(users.size(), 1U);
    EXPECT_EQ(users.front().display_name, "Renée 李");
}

} // namespace
} // namespace cha
