#pragma once

#include <chrono>
#include <cstddef>

namespace cha::web {

// Public fields deliberately make every deadline and bound injectable by tests.
// Application limits (session/queue/prompt/command/shutdown) are used natively.
// HTTP-only fields (pool, SSE, idle/orphan, body limit) are ignored by
// Application::open unless an HTTP adapter copies them in.
struct WebSettings {
    std::size_t session_limit{8};
    std::size_t http_request_headroom{4};
    std::size_t http_thread_pool_size{16};
    std::size_t http_pending_request_limit{32};
    std::size_t command_queue_capacity{64};
    std::size_t command_batch_size{8};
    std::size_t event_batch_size{64};
    std::chrono::milliseconds open_deadline{10000};
    std::chrono::milliseconds command_deadline{30000};
    std::chrono::milliseconds sse_heartbeat_interval{15000};
    std::chrono::milliseconds http_read_timeout{5000};
    std::chrono::milliseconds http_write_timeout{5000};
    std::chrono::milliseconds sse_drain_deadline{1000};
    std::chrono::milliseconds delete_deadline{10000};
    std::chrono::milliseconds idle_grace{30000};
    std::chrono::milliseconds orphan_limit{300000};
    std::size_t request_body_limit{65536};
    std::size_t prompt_limit{32768};
    std::chrono::milliseconds shutdown_grace{10000};
    std::size_t pending_append_byte_limit{65536};
    bool monotonic_event_sequence{false};
    bool browser_disconnect_lifetime{true};
};

} // namespace cha::web
