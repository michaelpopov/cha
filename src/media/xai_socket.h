#pragma once

#include "runtime/protocol.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha::media {

class XaiVoiceFailure : public std::runtime_error {
public:
    XaiVoiceFailure(ErrorCode code, std::string message);
    ErrorCode code;
};

struct XaiIncoming {
    bool binary = false;
    bool closed = false;
    std::string payload;
};

enum class XaiSendResult { sent, cancelled, timed_out, failed };

// One WebSocket connection. The worker is the only caller.
class XaiSocket {
public:
    virtual ~XaiSocket() = default;
    virtual void connect(
        const std::string& url,
        const std::string& authorization,
        std::chrono::steady_clock::time_point deadline,
        const std::function<bool()>& cancelled) = 0;
    virtual XaiSendResult send(
        std::string_view payload,
        bool binary,
        std::chrono::steady_clock::time_point deadline,
        const std::function<bool()>& cancelled) = 0;
    virtual std::optional<XaiIncoming> recv(std::chrono::milliseconds wait) = 0;
    virtual void close() = 0;
};

[[nodiscard]] std::unique_ptr<XaiSocket> make_xai_curl_socket();

inline constexpr std::string_view xai_websocket_required =
    "xAI voice input requires a curl build with WebSocket support";
inline constexpr std::string_view xai_authentication_failed =
    "xAI authentication failed.";
inline constexpr std::string_view xai_configuration_rejected =
    "xAI voice input configuration was rejected.";
inline constexpr std::string_view xai_connection_failed =
    "The xAI voice input connection failed.";
inline constexpr std::string_view xai_connection_closed =
    "The xAI voice input connection closed.";
inline constexpr std::string_view xai_completed_early =
    "xAI completed before the audio finished.";
inline constexpr std::string_view xai_timed_out = "xAI voice input timed out.";
inline constexpr std::string_view xai_pending_text_limit =
    "xAI transcript exceeded the pending text limit.";
inline constexpr std::string_view xai_operation_cancelled =
    "The operation was cancelled.";

} // namespace cha::media
