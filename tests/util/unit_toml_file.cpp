#include "util/toml_file.h"

#include "util/path_name.h"

#include <gtest/gtest.h>
#include <toml++/toml.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace cha {
namespace {

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

bool has_temporary_sibling(const std::filesystem::path& path) {
    const std::string prefix = path.filename().string() + ".temp.";
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        if (entry.path().filename().string().starts_with(prefix)) return true;
    }
    return false;
}

class TomlFileTest : public testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path()
            / ("cha_toml_file_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::permissions(
            directory_,
            std::filesystem::perms::owner_all,
            ignored);
        std::filesystem::remove_all(directory_, ignored);
    }

    std::filesystem::path path(std::string_view name = "app.toml") const {
        return directory_ / std::string(name);
    }

    std::filesystem::path directory_;
};

TEST_F(TomlFileTest, RewritesOneValueAndPreservesTheRest) {
    const std::filesystem::path file = path();
    std::ofstream(file)
        << "vault = \"A\"\n"
        << "name = \"kept\"\n"
        << "[web]\n"
        << "host = \"127.0.0.1\"\n"
        << "port = 8080\n";

    rewrite_toml_file(file, [](toml::table& table) {
        table.insert_or_assign("vault", std::string("B"));
    });

    const toml::table table = read_toml_file(file, "config file");
    EXPECT_EQ(table["vault"].value<std::string>(), "B");
    EXPECT_EQ(table["name"].value<std::string>(), "kept");
    ASSERT_TRUE(table["web"].is_table());
    EXPECT_EQ(table["web"]["host"].value<std::string>(), "127.0.0.1");
    EXPECT_EQ(table["web"]["port"].value<int>(), 8080);
    EXPECT_FALSE(has_temporary_sibling(file));
}

TEST_F(TomlFileTest, LeavesTheOriginalUnchangedWhenMutationThrows) {
    const std::filesystem::path file = path();
    const std::string original =
        "vault = \"A\"\n[logging]\nlevel = \"info\"\n";
    std::ofstream(file) << original;

    EXPECT_THROW(
        rewrite_toml_file(file, [](toml::table&) {
            throw std::runtime_error("mutation failed");
        }),
        std::runtime_error);
    EXPECT_EQ(file_bytes(file), original);
    EXPECT_FALSE(has_temporary_sibling(file));
}

#ifndef _WIN32
TEST_F(TomlFileTest, LeavesTheOriginalUnchangedWhenWritingFails) {
    const std::filesystem::path file = path();
    const std::string original = "vault = \"A\"\nkeep = true\n";
    std::ofstream(file) << original;

    ASSERT_EQ(
        ::chmod(directory_.c_str(), 0555),
        0);

    try {
        EXPECT_THROW(
            rewrite_toml_file(file, [](toml::table& table) {
                table.insert_or_assign("vault", std::string("B"));
            }),
            std::runtime_error);
        EXPECT_EQ(file_bytes(file), original);
        EXPECT_FALSE(has_temporary_sibling(file));
    } catch (...) {
        (void)::chmod(directory_.c_str(), 0755);
        throw;
    }
    ASSERT_EQ(::chmod(directory_.c_str(), 0755), 0);
}
#endif

} // namespace
} // namespace cha
