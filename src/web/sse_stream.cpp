#include "web/sse_stream.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <utility>

namespace cha::web {

std::string sse_frame(const SsePayload& payload) {
    return std::visit([](const auto& event) {
        using T = std::decay_t<decltype(event)>;
        constexpr std::string_view name = std::is_same_v<T, SnapshotEvent>
            ? "snapshot" : "append";
        return "event: " + std::string(name) + "\n" + "data: "
            + nlohmann::json(event).dump() + "\n\n";
    }, payload);
}

std::string sse_heartbeat() { return ": heartbeat\n\n"; }

SseStreamWriter::SseStreamWriter(
    std::shared_ptr<SseMailbox> mailbox,
    SseMailbox::Stream stream,
    std::chrono::milliseconds heartbeat_interval)
    : mailbox_(std::move(mailbox)), stream_(stream),
      heartbeat_interval_(heartbeat_interval) {}

bool SseStreamWriter::write(httplib::DataSink& sink) {
    try {
        const SseMailbox::Next next =
            mailbox_->next(stream_, heartbeat_interval_);
        if (!next.open) {
            sink.done();
            return true;
        }
        const std::string frame =
            next.payload ? sse_frame(*next.payload) : sse_heartbeat();
        if (!sink.write(frame.data(), frame.size())) return false;
        if (next.payload) mailbox_->written(stream_);
        return true;
    } catch (...) {
        // Presentation data and the network writer are outside the route
        // handler's exception boundary. Any failure here closes only this
        // stream; it must never unwind out of cpp-httplib's worker thread.
        return false;
    }
}

} // namespace cha::web
