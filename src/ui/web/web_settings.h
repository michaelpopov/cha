#pragma once

#include <chrono>
#include <cstddef>

namespace cha::web {

// Public fields deliberately make every deadline and bound injectable by tests.
struct WebSettings {
    std::size_t session_limit{8};
    // Server validation must keep enough request threads for every live
    // session's SSE stream plus command, snapshot, lobby, and asset headroom.
    std::size_t http_thread_pool_size{16};
    std::size_t http_pending_request_limit{32};
    std::size_t command_queue_capacity{64};
    std::chrono::milliseconds open_deadline{10000};
    std::chrono::milliseconds command_deadline{30000};
    std::chrono::milliseconds sse_heartbeat_interval{15000};
    std::chrono::milliseconds sse_write_timeout{5000};
    std::chrono::milliseconds sse_drain_deadline{1000};
    std::chrono::milliseconds idle_grace{30000};
    // Runtime configuration validation must require orphan_limit >= idle_grace.
    std::chrono::milliseconds orphan_limit{300000};
    std::chrono::milliseconds reload_retry_interval{2000};
    std::size_t request_body_limit{65536};
    std::size_t prompt_limit{32768};
    std::chrono::milliseconds shutdown_grace{10000};
};

} // namespace cha::web
