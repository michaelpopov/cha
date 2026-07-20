#pragma once

#include "session_repository.h"

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

    [[nodiscard]] std::optional<std::string> select_room(const std::vector<std::string>& rooms);
    [[nodiscard]] std::optional<Session> select_session(
        const std::vector<Session>& sessions,
        std::string_view error = {});
    [[nodiscard]] std::optional<std::string> prompt_session_name();

private:
    [[nodiscard]] std::optional<std::size_t> select(
        const std::string& title,
        const std::vector<std::string>& options,
        std::optional<std::size_t> emphasized_option = std::nullopt,
        std::string_view error = {});

    Terminal& terminal_;
};

} // namespace cha
