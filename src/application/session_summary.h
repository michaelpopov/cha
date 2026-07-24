#pragma once

#include <string>

namespace cha {

// Describes one selectable session without exposing its storage representation.
struct SessionSummary {
    std::string id;
    std::string label;
    std::string error;

    bool operator==(const SessionSummary&) const = default;
};

} // namespace cha
