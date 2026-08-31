#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace cha {

inline std::int64_t session_timestamp() noexcept {
    return std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace cha
