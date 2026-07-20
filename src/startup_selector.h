#pragma once

#include "workspace.h"

#include <optional>
#include <string>
#include <vector>

namespace cha {

class StartupSelector {
public:
    StartupSelector();
    ~StartupSelector();

    StartupSelector(const StartupSelector&) = delete;
    StartupSelector& operator=(const StartupSelector&) = delete;

    [[nodiscard]] std::optional<std::string> select_room(const std::vector<std::string>& rooms);
    [[nodiscard]] std::optional<Session> select_session(const std::vector<Session>& sessions);
    [[nodiscard]] std::optional<std::string> prompt_session_name();

private:
    [[nodiscard]] std::optional<std::size_t> select(const std::string& title, const std::vector<std::string>& options);
};

} // namespace cha
