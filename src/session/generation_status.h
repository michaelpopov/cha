#pragma once

#include "chat/ids.h"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

inline constexpr std::string_view generation_in_progress_notice =
    "Generation in progress; use the Stop button";

enum class ResponsePhase {
    waiting,
    reasoning,
    answering,
    stopping,
};

struct GenerationActivity {
    std::size_t id{};
    std::string tool;
    std::string argument;
    bool retrying{};
    bool operator==(const GenerationActivity&) const = default;
};

struct GenerationProgress {
    std::size_t searches_completed{};
    std::size_t pages_read{};
    std::vector<GenerationActivity> activities;
    bool operator==(const GenerationProgress&) const = default;
};

// What a front end needs to show about execution or abort cleanup in progress.
struct GenerationStatus {
    bool active{};
    std::optional<std::uint64_t> request_id;
    CharacterId character_id;
    std::string character_display_name;
    ResponsePhase phase{ResponsePhase::waiting};
    std::optional<GenerationProgress> progress;

    bool operator==(const GenerationStatus&) const = default;
};

} // namespace cha
