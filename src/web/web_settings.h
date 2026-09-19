#pragma once

#include <chrono>
#include <cstddef>

namespace cha::web {

// Public fields deliberately make every deadline and bound injectable by tests.
struct WebSettings {
    std::size_t session_limit{8};
    std::size_t command_queue_capacity{64};
    std::size_t command_batch_size{8};
    std::size_t event_batch_size{64};
    std::chrono::milliseconds open_deadline{10000};
    std::chrono::milliseconds command_deadline{30000};
    std::chrono::milliseconds delete_deadline{10000};
    std::size_t prompt_limit{32768};
    std::chrono::milliseconds shutdown_grace{10000};
    std::size_t pending_append_byte_limit{65536};
    bool monotonic_event_sequence{true};
};

} // namespace cha::web
