#pragma once

#include "web/sse_mailbox.h"

#include <chrono>
#include <memory>
#include <string>

namespace httplib { class DataSink; }

namespace cha::web {

[[nodiscard]] std::string sse_frame(const SsePayload& payload);
[[nodiscard]] std::string sse_heartbeat();
// The last frame a displaced stream writes, so the device that just lost the
// session parks instead of reconnecting and taking it straight back.
[[nodiscard]] std::string sse_superseded();

class SseStreamWriter {
public:
    SseStreamWriter(
        std::shared_ptr<SseMailbox> mailbox,
        SseMailbox::Stream stream,
        std::chrono::milliseconds heartbeat_interval);
    [[nodiscard]] bool write(httplib::DataSink& sink);

private:
    std::shared_ptr<SseMailbox> mailbox_;
    SseMailbox::Stream stream_;
    std::chrono::milliseconds heartbeat_interval_;
};

} // namespace cha::web
