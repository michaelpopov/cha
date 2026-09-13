#include "providers/api_key_store.h"
#include "support/test_workspace.h"
#include "util/environment.h"
#include "util/toml_file.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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
        for (std::size_t index = 0; index != r2_environment.size(); ++index) {
            if (const char* value = std::getenv(r2_environment[index])) {
                previous_r2_environment[index] = value;
            }
            ASSERT_TRUE(unset_environment_variable(r2_environment[index]));
        }
        directory = std::filesystem::temp_directory_path()
            / ("cha_api_keys_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        path = directory / "api-keys.json";
        database = test::import_test_database(
            workspace.root(), directory / "workspace.sqlite3");
        config = WorkspaceConfigStore::open(database);
    }

    void TearDown() override {
        config.reset();
        std::filesystem::remove_all(directory);
        for (std::size_t index = 0; index != r2_environment.size(); ++index) {
            if (previous_r2_environment[index]) {
                (void)set_environment_variable(
                    r2_environment[index], *previous_r2_environment[index]);
            } else {
                (void)unset_environment_variable(r2_environment[index]);
            }
        }
    }

    void reopen() {
        config.reset();
        config = WorkspaceConfigStore::open(database);
    }

    test::TestWorkspace workspace;
    std::filesystem::path directory;
    std::filesystem::path path;
    std::filesystem::path database;
    std::unique_ptr<WorkspaceConfigStore> config;
    const std::array<const char*, 3> r2_environment{
        "CHA_R2_URL",
        "CHA_R2_ACCESS_KEY_ID",
        "CHA_R2_SECRET_ACCESS_KEY",
    };
    std::array<std::optional<std::string>, 3> previous_r2_environment;
};

TEST_F(ApiKeyStoreTest, CreatesUpdatesReloadsAndRemovesASecret) {
    ApiKeyStore store(*config, path);
    const ApiKeyInfo created = store.create("OpenRouter", "secret-one");
    EXPECT_EQ(created.id, "api_key_1");
    EXPECT_EQ(created.display_name, "OpenRouter");
    EXPECT_TRUE(created.has_value);
    EXPECT_EQ(store.value(created.id), "secret-one");
    ASSERT_TRUE(store.find_by_name("OpenRouter"));
    EXPECT_EQ(store.find_by_name("OpenRouter")->id, created.id);
    EXPECT_EQ(store.value_by_name("OpenRouter"), "secret-one");

    const ApiKeyInfo renamed = store.rename(created.id, "Router");
    EXPECT_EQ(renamed.display_name, "Router");
    EXPECT_FALSE(store.find_by_name("OpenRouter"));
    EXPECT_EQ(store.value_by_name("Router"), "secret-one");
    EXPECT_EQ(store.replace(created.id, "secret-two").id, created.id);

    reopen();
    ApiKeyStore reopened(*config, path);
    ASSERT_EQ(reopened.list().size(), 1U);
    EXPECT_EQ(reopened.list().front().display_name, "Router");
    EXPECT_EQ(reopened.value(created.id), "secret-two");

#ifndef _WIN32
    struct stat info {};
    const std::filesystem::path stored = config->workspace_path()
        / "system" / "keys" / created.id / "config.toml";
    ASSERT_EQ(::stat(stored.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, static_cast<mode_t>(0600));
#endif

    reopened.remove(created.id);
    EXPECT_TRUE(reopened.list().empty());
    EXPECT_THROW((void)reopened.value(created.id), std::runtime_error);
}

TEST_F(ApiKeyStoreTest, NeverReusesRemovedIds) {
    ApiKeyStore store(*config, path);
    const ApiKeyInfo first = store.create("First", "secret-one");
    const ApiKeyInfo second = store.create("Second", "secret-two");
    store.remove(first.id);

    const ApiKeyInfo third = store.create("Third", "secret-three");
    EXPECT_EQ(third.id, "api_key_3");

    reopen();
    ApiKeyStore reopened(*config, path);
    reopened.remove(second.id);
    reopened.remove(third.id);
    EXPECT_EQ(
        reopened.create("Fourth", "secret-four").id,
        "api_key_4");
}

TEST_F(ApiKeyStoreTest, CreatesUpdatesReloadsAndRemovesR2Credentials) {
    ApiKeyStore store(*config, path);
    EXPECT_THROW(
        (void)store.save_r2("R2", "https://account.example/bucket",
            "access-one", std::nullopt),
        std::invalid_argument);

    const R2StorageInfo created = store.save_r2(
        "R2", "https://account.example/bucket", "access-one", "secret-one");
    EXPECT_EQ(created.id, "api_key_1");
    EXPECT_TRUE(created.has_secret_key);
    EXPECT_EQ(store.create("Models", "model-secret").id, "api_key_2");

    const R2StorageInfo updated = store.save_r2(
        "Backups", "https://new.example/bucket", "access-two", std::nullopt);
    EXPECT_EQ(updated.id, created.id);
    EXPECT_EQ(updated.display_name, "Backups");
    ASSERT_TRUE(store.r2());
    EXPECT_EQ(store.r2()->secret_key, "secret-one");

    reopen();
    ApiKeyStore reopened(*config, path);
    ASSERT_TRUE(reopened.r2_info());
    EXPECT_EQ(reopened.r2_info()->url, "https://new.example/bucket");
    EXPECT_EQ(reopened.r2()->access_key_id, "access-two");
    EXPECT_EQ(reopened.r2()->secret_key, "secret-one");

    reopened.remove_r2();
    EXPECT_FALSE(reopened.r2());
    EXPECT_THROW(reopened.remove_r2(), std::out_of_range);
}

TEST_F(ApiKeyStoreTest, MigratesTheLegacyFileWithoutReusingIds) {
    const std::string legacy =
        R"({"version":1,"keys":{"api_key_7":{"display_name":"Old","value":"secret"}}})";
    {
        std::ofstream output(path);
        output << legacy;
    }

    ApiKeyStore store(*config, path);
    std::ifstream legacy_input(path, std::ios::binary);
    const std::string legacy_bytes{
        std::istreambuf_iterator<char>(legacy_input),
        std::istreambuf_iterator<char>()};
    EXPECT_EQ(legacy_bytes, legacy);
    const std::filesystem::path migrated = config->workspace_path()
        / "system" / "keys" / "api_key_7" / "config.toml";
    const toml::table migrated_config =
        read_toml_file(migrated, "migrated key");
    EXPECT_EQ(migrated_config["type"].value<std::string>(), "models");
    EXPECT_EQ(migrated_config["value"].value<std::string>(), "secret");
    store.remove("api_key_7");
    EXPECT_EQ(store.create("New", "new-secret").id, "api_key_8");
}

TEST_F(ApiKeyStoreTest, RejectsMalformedFilesAndInvalidValues) {
    {
        std::ofstream output(path);
        output << R"({"version":1,"keys":{"not-a-key":{"display_name":"Bad","value":"secret"}}})";
    }
    EXPECT_THROW((void)ApiKeyStore(*config, path), std::runtime_error);

    std::filesystem::remove(path);
    ApiKeyStore store(*config, path);
    EXPECT_THROW(store.create("", "secret"), std::invalid_argument);
    EXPECT_THROW(store.create("Valid", ""), std::invalid_argument);
    EXPECT_THROW(store.create("Valid", "line\nbreak"), std::invalid_argument);
    EXPECT_THROW((void)store.value_by_name("Missing"), std::runtime_error);

    std::filesystem::remove(path);
    {
        std::ofstream output(path);
        output << R"({"version":2,"next_id":1,"keys":{"api_key_1":{"display_name":"Bad","value":"secret"}}})";
    }
    EXPECT_THROW((void)ApiKeyStore(*config, path), std::runtime_error);
}

} // namespace
} // namespace cha
