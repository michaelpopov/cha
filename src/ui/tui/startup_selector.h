#pragma once

#include "session/workspace.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class Terminal;

// The screens shown before a chat begins: choose a forum, then choose or name a session. It draws
// temporary selection lists on a borrowed Terminal and works only from the forum names and
// SessionSummary values it is handed by Workspace, so selection never reaches into session storage.
class StartupSelector {
public:
    explicit StartupSelector(Terminal& terminal);

    StartupSelector(const StartupSelector&) = delete;
    StartupSelector& operator=(const StartupSelector&) = delete;

    // Converts roster values to the presentation-safe labels consumed by the
    // selector. Keeping this mapping independent of curses makes the startup
    // boundary directly testable.
    [[nodiscard]] static std::vector<std::string> user_display_names(
        const UserRoster& users);
    [[nodiscard]] static std::optional<User> selected_user(
        const UserRoster& users,
        // A selection index must have come from select() over this roster.
        std::optional<std::size_t> selection);
    std::optional<User> select_user(const UserRoster& users);
    std::optional<std::string> select_forum(const std::vector<Forum>& forums);
    std::optional<SessionSummary> select_session(
        const std::vector<SessionSummary>& sessions,
        std::string_view error = {});
    std::optional<std::string> prompt_session_name();

private:
    std::optional<std::size_t> select(
        const std::string& title,
        const std::vector<std::string>& options,
        std::optional<std::size_t> emphasized_option = std::nullopt,
        std::string_view error = {});

    Terminal& terminal_;
};

} // namespace cha
