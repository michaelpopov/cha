#include "util/path_name.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace cha {
namespace {

TEST(Utf8Path, PreservesUtf8PathBytes) {
    const std::filesystem::path path(
        u8"workspace/na\u00efve/\u6771\u4eac.sqlite3");

    EXPECT_EQ(
        utf8_path(path),
        "workspace/na\xc3\xafve/\xe6\x9d\xb1\xe4\xba\xac.sqlite3");
}

TEST(Utf8Path, ConstructsNativePathsFromUtf8Text) {
    EXPECT_EQ(
        utf8_path(path_from_utf8(
            "workspace/na\xc3\xafve/\xe6\x9d\xb1\xe4\xba\xac.sqlite3")),
        "workspace/na\xc3\xafve/\xe6\x9d\xb1\xe4\xba\xac.sqlite3");
}

#ifdef _WIN32
TEST(Utf8Path, ConvertsWindowsCommandLineText) {
    EXPECT_EQ(
        utf8_from_wide(L"na\u00efve \u6771\u4eac"),
        "na\xc3\xafve \xe6\x9d\xb1\xe4\xba\xac");
}
#endif

} // namespace
} // namespace cha
