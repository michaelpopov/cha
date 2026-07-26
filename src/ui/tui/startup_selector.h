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
