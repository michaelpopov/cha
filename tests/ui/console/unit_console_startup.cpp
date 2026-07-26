#include "session/session_database.h"
#include "ui/console/console_startup.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <variant>

namespace cha {
namespace {

class ListingWorkspace {
public:
    ListingWorkspace()
        : root(std::filesystem::temp_directory_path()
            / ("cha_console_startup_"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count()))) {
        const auto room = root / "rooms" / "hall";
        const auto sessions = room / "sessions";
        std::filesystem::create_directories(root / "personas");
        std::filesystem::create_directories(sessions);
        write(root / "rooms" / "rooms.list", "hall\n");
        write(room / "personas.list", "Guide\n");
        if (!create_session_database(
                sessions / "valid.sqlite3",
                {
                    .id = "valid",
                    .room = "hall",
                    .label = "Design\treview",
                })) {
            throw std::runtime_error("Failed to create valid test session");
        }
        write(sessions / "broken.sqlite3", "not sqlite");
    }

    ~ListingWorkspace() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    static void write(
        const std::filesystem::path& path,
        std::string_view value) {
        std::ofstream file(path);
        file << value;
    }

    std::filesystem::path root;
};

std::variant<ConsoleOptions, ArgumentError> parse(
    std::initializer_list<const char*> arguments) {
    std::vector<const char*> argv(arguments);
    return parse_console_arguments(
        static_cast<int>(argv.size()),
        argv.data());
}

TEST(ConsoleStartup, ParsesSelectionAndColorOptions) {
    const auto parsed = parse({
        "chacon",
        "--room", "hall",
        "--session", "saved",
        "--color=always",
    });
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(parsed));
    const ConsoleOptions& options = std::get<ConsoleOptions>(parsed);
    EXPECT_EQ(options.room, "hall");
    EXPECT_EQ(options.session_id, "saved");
    EXPECT_EQ(options.color, ColorMode::always);

    const auto fresh = parse({"chacon", "--room", "hall"});
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(fresh));
    ASSERT_TRUE(std::get<ConsoleOptions>(fresh).new_label.has_value());
    EXPECT_TRUE(std::get<ConsoleOptions>(fresh).new_label->empty());
}

TEST(ConsoleStartup, RejectsUsageErrorsWithCodeTwo) {
    for (const auto& parsed : {
        parse({"chacon"}),
        parse({"chacon", "--list-sessions"}),
        parse({"chacon", "--room"}),
        parse({"chacon", "--unknown"}),
        parse({"chacon", "operand"}),
        parse({"chacon", "--room", "hall", "--session", ""}),
        parse({
            "chacon", "--room", "hall",
            "--session", "x", "--new", "y",
        }),
        parse({"chacon", "--room", "hall", "--color=sometimes"}),
    }) {
        ASSERT_TRUE(std::holds_alternative<ArgumentError>(parsed));
        EXPECT_EQ(std::get<ArgumentError>(parsed).exit_code, 2);
    }
}

TEST(ConsoleStartup, RoomListingUsesWorkspaceOrder) {
    ListingWorkspace fixture;
    const Workspace workspace(fixture.root);
    std::ostringstream output;
    write_room_listing(workspace, output);
    EXPECT_EQ(output.str(), "hall\n");
}

TEST(ConsoleStartup, SessionListingIsPlainStableAndIncludesErrors) {
    ListingWorkspace fixture;
    const Workspace workspace(fixture.root);
    std::ostringstream output;
    write_session_listing(workspace, "hall", output);
    const std::string listing = output.str();

    EXPECT_NE(
        listing.find("valid\tDesign review\t\n"),
        std::string::npos);
    EXPECT_NE(
        listing.find("broken\tbroken [invalid database]\t"),
        std::string::npos);
    EXPECT_EQ(listing.find('\x1b'), std::string::npos);
}

TEST(ConsoleStartup, ListRoomsWinsOverSelectionFlags) {
    const auto parsed = parse({
        "chacon",
        "--list-rooms",
        "--room", "ignored",
        "--session", "ignored",
        "--new", "ignored",
    });
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(parsed));
    EXPECT_TRUE(std::get<ConsoleOptions>(parsed).list_rooms);

    const auto empty_session = parse({
        "chacon",
        "--list-rooms",
        "--session", "",
    });
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(empty_session));
    EXPECT_TRUE(std::get<ConsoleOptions>(empty_session).list_rooms);

    const auto session_listing = parse({
        "chacon",
        "--room", "hall",
        "--list-sessions",
        "--session", "",
    });
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(session_listing));
    EXPECT_TRUE(std::get<ConsoleOptions>(session_listing).list_sessions);
}

} // namespace
} // namespace cha
