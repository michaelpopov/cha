#include "providers/api_key_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace cha {
namespace {

class ApiKeyStoreTest : public testing::Test {
protected:
    void SetUp() override {
        directory = std::filesystem::temp_directory_path()
            / ("cha_api_keys_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        path = directory / "api-keys.json";
    }

    void TearDown() override { std::filesystem::remove_all(directory); }

    std::filesystem::path directory;
    std::filesystem::path path;
};

TEST_F(ApiKeyStoreTest, CreatesUpdatesReloadsAndRemovesASecret) {
    ApiKeyStore store(path);
    const ApiKeyInfo created = store.create("OpenRouter", "secret-one");
    EXPECT_EQ(created.id, "api_key_1");
    EXPECT_EQ(created.display_name, "OpenRouter");
    EXPECT_TRUE(created.has_value);
    EXPECT_EQ(store.value(created.id), "secret-one");

    const ApiKeyInfo renamed = store.rename(created.id, "Router");
    EXPECT_EQ(renamed.display_name, "Router");
    EXPECT_EQ(store.replace(created.id, "secret-two").id, created.id);

    ApiKeyStore reopened(path);
    ASSERT_EQ(reopened.list().size(), 1U);
    EXPECT_EQ(reopened.list().front().display_name, "Router");
    EXPECT_EQ(reopened.value(created.id), "secret-two");

#ifndef _WIN32
    struct stat info {};
    ASSERT_EQ(::stat(path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, static_cast<mode_t>(0600));
#endif

    reopened.remove(created.id);
    EXPECT_TRUE(reopened.list().empty());
    EXPECT_THROW((void)reopened.value(created.id), std::runtime_error);
}

TEST_F(ApiKeyStoreTest, NeverReusesRemovedIds) {
    ApiKeyStore store(path);
    const ApiKeyInfo first = store.create("First", "secret-one");
    const ApiKeyInfo second = store.create("Second", "secret-two");
    store.remove(first.id);

    const ApiKeyInfo third = store.create("Third", "secret-three");
    EXPECT_EQ(third.id, "api_key_3");

    ApiKeyStore reopened(path);
    reopened.remove(second.id);
    reopened.remove(third.id);
    EXPECT_EQ(
        reopened.create("Fourth", "secret-four").id,
        "api_key_4");
}

TEST_F(ApiKeyStoreTest, MigratesTheLegacyFileWithoutReusingIds) {
    {
        std::ofstream output(path);
        output << R"({"version":1,"keys":{"api_key_7":{"display_name":"Old","value":"secret"}}})";
    }

    ApiKeyStore store(path);
    store.remove("api_key_7");
    EXPECT_EQ(store.create("New", "new-secret").id, "api_key_8");
}

TEST_F(ApiKeyStoreTest, RejectsMalformedFilesAndInvalidValues) {
    {
        std::ofstream output(path);
        output << R"({"version":1,"keys":{"not-a-key":{"display_name":"Bad","value":"secret"}}})";
    }
    EXPECT_THROW((void)ApiKeyStore(path), std::runtime_error);

    std::filesystem::remove(path);
    ApiKeyStore store(path);
    EXPECT_THROW(store.create("", "secret"), std::invalid_argument);
    EXPECT_THROW(store.create("Valid", ""), std::invalid_argument);
    EXPECT_THROW(store.create("Valid", "line\nbreak"), std::invalid_argument);

    std::filesystem::remove(path);
    {
        std::ofstream output(path);
        output << R"({"version":2,"next_id":1,"keys":{"api_key_1":{"display_name":"Bad","value":"secret"}}})";
    }
    EXPECT_THROW((void)ApiKeyStore(path), std::runtime_error);
}

} // namespace
} // namespace cha
