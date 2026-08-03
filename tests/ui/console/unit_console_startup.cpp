#include "session/session_database.h"
#include "support/test_notifier.h"
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
        const auto forum = root / "forums" / "hall";
        const auto sessions = forum / "sessions";
        std::filesystem::create_directories(
            root / "characters" / "Guide");
        std::filesystem::create_directories(forum / "members" / "Guide");
        std::filesystem::create_directories(root / "personas" / "reader");
        std::filesystem::create_directories(sessions);
        write(
            root / "app.toml",
            "host = \"127.0.0.1\"\nport = 8080\n"
            "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n");
        write(forum / "config.toml", "display_name = \"The Hall\"\n");
        write(forum / "FORUM.md", "Forum instructions\n");
        write(
            forum / "members" / "character_defaults.toml",
            "host = \"127.0.0.1\"\nport = 8080\n");
        write(
            root / "characters" / "Guide" / "character.toml",
            "display_name = \"Guide\"\n");
        write(
            root / "characters" / "Guide" / "CHARACTER.md",
            "Character instructions\n");
        write(root / "personas" / "reader" / "persona.toml", "display_name = \"Reader\"\n");
        if (!create_session_database(
                sessions / "valid.sqlite3",
                {
                    .id = "valid",
                    .forum = "hall",
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
        "--persona", "reader",
        "--forum", "hall",
        "--session", "saved",
        "--color=always",
    });
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(parsed));
    const ConsoleOptions& options = std::get<ConsoleOptions>(parsed);
    EXPECT_EQ(options.forum, "hall");
    EXPECT_EQ(options.persona, "reader");
    EXPECT_EQ(options.session_id, "saved");
    EXPECT_EQ(options.color, ColorMode::always);

    const auto fresh = parse({"chacon", "--persona", "reader", "--forum", "hall"});
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(fresh));
    ASSERT_TRUE(std::get<ConsoleOptions>(fresh).new_label.has_value());
    EXPECT_TRUE(std::get<ConsoleOptions>(fresh).new_label->empty());

    const auto checked = parse({"chacon", "--forum", "hall", "--check"});
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(checked));
    EXPECT_TRUE(std::get<ConsoleOptions>(checked).check_forum);
    EXPECT_FALSE(std::get<ConsoleOptions>(checked).new_label.has_value());
}

TEST(ConsoleStartup, RejectsUsageErrorsWithCodeTwo) {
    for (const auto& parsed : {
        parse({"chacon"}),
        parse({"chacon", "--list-sessions"}),
        parse({"chacon", "--forum"}),
        parse({"chacon", "--unknown"}),
        parse({"chacon", "operand"}),
        parse({"chacon", "--forum", "hall", "--session", ""}),
        parse({"chacon", "--persona", "", "--forum", "hall"}),
        parse({"chacon", "--forum", "hall"}),
        parse({
            "chacon", "--forum", "hall",
            "--session", "x", "--new", "y",
        }),
        parse({
            "chacon", "--forum", "hall",
            "--check", "--session", "x",
        }),
        parse({
            "chacon", "--forum", "hall",
            "--check", "--new", "x",
        }),
        parse({
            "chacon", "--forum", "hall",
            "--check", "--list-sessions",
        }),
        parse({"chacon", "--forum", "hall", "--color=sometimes"}),
        parse({"chacon", "--persona", "reader", "--list-forums"}),
        parse({"chacon", "--persona", "reader", "--forum", "hall", "--list-sessions"}),
        parse({"chacon", "--persona", "reader", "--forum", "hall", "--check"}),
    }) {
        ASSERT_TRUE(std::holds_alternative<ArgumentError>(parsed));
        EXPECT_EQ(std::get<ArgumentError>(parsed).exit_code, 2);
    }
}

TEST(ConsoleStartup, RejectsAnEmptyExplicitPersonaID) {
    const auto parsed = parse({"chacon", "--persona", "", "--forum", "hall"});

    ASSERT_TRUE(std::holds_alternative<ArgumentError>(parsed));
    EXPECT_EQ(std::get<ArgumentError>(parsed).exit_code, 2);
    EXPECT_EQ(std::get<ArgumentError>(parsed).message, "--persona requires a persona ID");
}

TEST(ConsoleStartup, ForumListingUsesFilesystemNameOrder) {
    ListingWorkspace fixture;
    const Workspace workspace(fixture.root);
    std::ostringstream output;
    write_forum_listing(workspace, output);
    EXPECT_EQ(output.str(), "The Hall\n");
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

TEST(ConsoleStartup, ForumCheckIsPlainAndDoesNotCreateASession) {
    ListingWorkspace fixture;
    const Workspace workspace(fixture.root);
    std::ostringstream output;
    const std::vector<SessionSummary> sessions_before =
        workspace.sessions("hall");

    write_forum_check(workspace, "hall", output);

    EXPECT_EQ(output.str(), "Forum 'hall' is valid (1 character).\n");
    EXPECT_EQ(workspace.sessions("hall"), sessions_before);
}

TEST(ConsoleStartup, ListForumsWinsOverSelectionFlags) {
    const auto parsed = parse({
        "chacon",
        "--list-forums",
        "--forum", "ignored",
        "--session", "ignored",
        "--new", "ignored",
        "--check",
    });
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(parsed));
    EXPECT_TRUE(std::get<ConsoleOptions>(parsed).list_forums);

    const auto empty_session = parse({
        "chacon",
        "--list-forums",
        "--session", "",
    });
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(empty_session));
    EXPECT_TRUE(std::get<ConsoleOptions>(empty_session).list_forums);

    const auto session_listing = parse({
        "chacon",
        "--forum", "hall",
        "--list-sessions",
        "--session", "",
    });
    ASSERT_TRUE(std::holds_alternative<ConsoleOptions>(session_listing));
    EXPECT_TRUE(std::get<ConsoleOptions>(session_listing).list_sessions);
}

TEST(ConsoleStartup, OpensExistingSessionWithItsDescriptor) {
    ListingWorkspace fixture;
    const Workspace workspace(fixture.root);
    test::NoopNotifier notifier;
    const ConsoleOptions options{
        .persona = "reader",
        .forum = "hall",
        .session_id = "valid",
    };

    OpenedSession opened = open_console_session(workspace, options, notifier);

    EXPECT_EQ(
        opened.descriptor,
        (SessionDescriptor{
            .identity = {"hall", "valid"},
            .forum_display_name = "The Hall",
            .session_label = "Design\treview",
        }));
    ASSERT_TRUE(opened.controller);
    opened.controller->shutdown();
}

TEST(ConsoleStartup, CreatesSessionWithResolvedDescriptor) {
    ListingWorkspace fixture;
    const Workspace workspace(fixture.root);
    test::NoopNotifier notifier;
    const ConsoleOptions options{
        .persona = "reader",
        .forum = "hall",
        .new_label = "New session",
    };

    OpenedSession opened = open_console_session(workspace, options, notifier);

    EXPECT_EQ(opened.descriptor.identity.forum_id, "hall");
    EXPECT_FALSE(opened.descriptor.identity.session_id.empty());
    EXPECT_EQ(opened.descriptor.forum_display_name, "The Hall");
    EXPECT_EQ(opened.descriptor.session_label, "New session");
    ASSERT_TRUE(opened.controller);
    opened.controller->shutdown();
}

} // namespace
} // namespace cha
