#include "session/session_lease.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#ifndef _WIN32
#include "support/lease_test_process.h"
#endif

namespace cha {
namespace {

class SessionLeaseTest : public testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path()
            / ("cha_session_lease_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override {
        std::filesystem::remove_all(directory_);
    }

    std::filesystem::path database_path(std::string_view name = "session") const {
        return directory_ / (std::string(name) + ".sqlite3");
    }

    std::filesystem::path directory_;
};

TEST_F(SessionLeaseTest, UsesACompanionPathAndIgnoresAnUnlockedExistingFile) {
    const std::filesystem::path database = database_path();
    const std::filesystem::path companion =
        database_path().string() + ".cha-lock";
    EXPECT_EQ(SessionLease::companion_path(database), companion);

    std::ofstream(companion) << "left behind after a prior run";
    SessionLease lease = SessionLease::acquire(database);
    EXPECT_TRUE(lease.active());
}

TEST_F(SessionLeaseTest, HoldsIndependentLeasesWithoutCrossRelease) {
    const std::filesystem::path first_path = database_path("first");
    const std::filesystem::path second_path = database_path("second");
    SessionLease first = SessionLease::acquire(first_path);
    SessionLease second = SessionLease::acquire(second_path);
    first = SessionLease::inactive_for_testing();
    EXPECT_FALSE(first.active());
    EXPECT_TRUE(second.active());
#ifndef _WIN32
    EXPECT_EQ(
        test::probe_lease(first_path),
        test::LeaseProbeResult::acquired);
    EXPECT_EQ(
        test::probe_lease(second_path),
        test::LeaseProbeResult::busy);
#endif
}

TEST_F(SessionLeaseTest, ReportsOpenFailuresWithoutCallingThemBusy) {
    const std::filesystem::path impossible =
        directory_ / "missing" / "session.sqlite3";
    EXPECT_THROW((void)SessionLease::acquire(impossible), std::system_error);
}

TEST_F(SessionLeaseTest, RejectsMalformedDatabasePaths) {
    EXPECT_THROW(
        (void)SessionLease::companion_path({}),
        std::invalid_argument);
}

#ifndef _WIN32
TEST_F(SessionLeaseTest, RejectsSameProcessReacquireWithoutReleasingOwner) {
    const std::filesystem::path database = database_path();
    SessionLease owner = SessionLease::acquire(database);
    try {
        (void)SessionLease::acquire(database);
        FAIL() << "expected the second lease acquisition to report busy";
    } catch (const SessionBusyError& error) {
        EXPECT_NE(
            std::string(error.what()).find("'session'"),
            std::string::npos);
    }

    EXPECT_EQ(
        test::probe_lease(database),
        test::LeaseProbeResult::busy);
    EXPECT_TRUE(owner.active());
}

TEST_F(SessionLeaseTest, RejectsAnotherProcessAndReleasesAfterOwnerTermination) {
    const std::filesystem::path database = database_path();
    test::LeaseHolderProcess holder(database);

    EXPECT_THROW((void)SessionLease::acquire(database), SessionBusyError);

    holder.terminate();

    SessionLease released = SessionLease::acquire(database);
    EXPECT_TRUE(released.active());
}
#endif

} // namespace
} // namespace cha
