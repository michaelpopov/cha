#pragma once

#include <map>
#include <optional>
#include <string>

namespace cha {

struct SessionCharacterSettings {
    std::optional<std::string> provider_id;
    std::optional<std::string> reasoning_effort;

    bool operator==(const SessionCharacterSettings&) const = default;
};

using SessionCharacterSettingsMap =
    std::map<std::string, SessionCharacterSettings, std::less<>>;

} // namespace cha
