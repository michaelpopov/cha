#pragma once

#include "bridge/bridge_protocol.h"

namespace cha::app {
class Application;
}

namespace cha::bridge {

// A disengaged result means this dispatcher does not handle the method.
// A JSON null is a handled response, such as an unconfigured setting.
[[nodiscard]] std::optional<nlohmann::json> dispatch_workspace_operation(
    app::Application& application,
    Method method,
    const nlohmann::json& params,
    std::uint64_t epoch);
[[nodiscard]] std::optional<nlohmann::json> dispatch_settings_operation(
    app::Application& application,
    Method method,
    const nlohmann::json& params,
    std::uint64_t epoch);

} // namespace cha::bridge
