#include "session/catalog_lease.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

namespace cha {
namespace {

class CatalogLeaseTest : public testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() / ("cha_catalog_lease_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    }
    void TearDown() override { std::filesystem::remove_all(directory_); }
    std::filesystem::path directory_;
};

TEST_F(CatalogLeaseTest, CreatesAPermanentPrivateLockFile) {
    {
        CatalogLease lease = CatalogLease::acquire(directory_);
        EXPECT_TRUE(lease.active());
    }
    EXPECT_TRUE(std::filesystem::is_regular_file(CatalogLease::lock_path(directory_)));
    EXPECT_TRUE(CatalogLease::acquire(directory_).active());
}

TEST_F(CatalogLeaseTest, TimesOutUsingInjectedClockAndBackoff) {
    CatalogLease owner = CatalogLease::acquire(directory_);
    auto now = std::chrono::steady_clock::time_point{};
    EXPECT_THROW(
        (void)CatalogLease::acquire(directory_, std::chrono::milliseconds(2),
            [&now] { return now; }, [&now] { now += std::chrono::milliseconds(1); }),
        CatalogBusyError);
}

} // namespace
} // namespace cha
