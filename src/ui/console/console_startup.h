#pragma once

#include "session/workspace.h"

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace cha {

class WakeNotifier;

// Controls transcript styling only; machine-readable listings remain plain.
enum class ColorMode {
    automatic,
    always,
    never,
};

// The filesystem-independent result of parsing chacon's selection, listing,
// and color flags. The startup helpers later resolve it through Workspace.
struct ConsoleOptions {
    std::string room;
    std::string session_id;
    std::optional<std::string> new_label;
    bool list_rooms{};
    bool list_sessions{};
    ColorMode color{ColorMode::automatic};
};

// A command-line usage failure kept distinct from runtime workspace errors so
// console_main can preserve the exit-code contract.
struct ArgumentError {
    std::string message;
    int exit_code{2};
};

// Parses argv without touching the workspace, returning exit-code-2 failures
// as values rather than throwing.
std::variant<ConsoleOptions, ArgumentError> parse_console_arguments(
    int argc,
    const char* const* argv);

// Writes stable, sanitized, uncolored records for scripts.
void write_room_listing(const Workspace& workspace, std::ostream& out);
void write_session_listing(
    const Workspace& workspace,
    const std::string& room,
    std::ostream& out);

// The session the console is about to run: the controller it drives, and the
// resolved session ID, which lets a front end report a newly created session
// without rescanning the catalog.
struct ConsoleSelection {
    std::unique_ptr<SessionController> controller;
    std::string session_id;
};

// Resolves a validated selection through Workspace. Throws std::runtime_error
// for a missing room or session and for a session whose summary carries an
// error, so console_main can report those as runtime rather than usage failures.
[[nodiscard]] ConsoleSelection open_console_session(
    const Workspace& workspace,
    const ConsoleOptions& options,
    WakeNotifier& notifier);

} // namespace cha
