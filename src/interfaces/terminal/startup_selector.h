#pragma once

#include "application/session_summary.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class Terminal;

// Owns the temporary ncurses screens used to choose a room and session before chat starts.
class StartupSelector {
public:
    explicit StartupSelector(Terminal& terminal);

    StartupSelector(const StartupSelector&) = delete;
    StartupSelector& operator=(const StartupSelector&) = delete;

    std::optional<std::string> select_room(const std::vector<std::string>& rooms);
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
