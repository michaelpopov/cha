#include "media/xai_socket.h"

#include "media/xai_transcript.h"
#include "util/curl.h"
#include "util/logging.h"

#include <curl/curl.h>

#include <chrono>
#include <deque>
#include <memory>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace cha::media {
namespace {

using clock = std::chrono::steady_clock;

int wait_budget_ms(clock::time_point deadline) {
    const auto now = clock::now();
    if (now >= deadline) return 0;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    if (left.count() > 100) return 100;
    return static_cast<int>(left.count());
}

std::string_view scheme_of(std::string_view url) {
    if (url.starts_with("wss://")) return "wss";
    if (url.starts_with("ws://")) return "ws";
    return {};
}

class XaiCurlSocket final : public XaiSocket {
public:
    XaiCurlSocket() = default;
    ~XaiCurlSocket() override { close(); }

    XaiCurlSocket(const XaiCurlSocket&) = delete;
    XaiCurlSocket& operator=(const XaiCurlSocket&) = delete;

    void connect(
        const std::string& url,
        const std::string& authorization,
        clock::time_point deadline,
        const std::function<bool()>& cancelled) override {
        const std::string_view scheme = scheme_of(url);
        if (scheme != "ws" && scheme != "wss") {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_connection_failed));
        }
        if (!curl_supports_websocket_scheme(scheme)) {
#ifdef __APPLE__
            log_error("xAI voice input curl build is missing WebSocket support.");
#endif
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_websocket_required));
        }
        curl_ = CurlHandle();
        headers_ = std::make_unique<CurlHeaders>();
        headers_->append(authorization);
        const auto require = [&](CURLcode result) {
            if (result != CURLE_OK) {
                log_warn(
                    "xAI voice input setup failed (curl "
                    + std::to_string(result) + ")");
                throw XaiVoiceFailure(
                    ErrorCode::internal_error, std::string(xai_connection_failed));
            }
        };
        require(curl_easy_setopt(curl_.get(), CURLOPT_URL, url.c_str()));
        require(curl_easy_setopt(curl_.get(), CURLOPT_CONNECT_ONLY, 2L));
        require(curl_easy_setopt(curl_.get(), CURLOPT_FOLLOWLOCATION, 0L));
        require(curl_easy_setopt(curl_.get(), CURLOPT_MAXREDIRS, 0L));
        require(curl_easy_setopt(curl_.get(), CURLOPT_PROTOCOLS_STR, "WS,WSS"));
        require(curl_easy_setopt(curl_.get(), CURLOPT_SSL_VERIFYPEER, 1L));
        require(curl_easy_setopt(curl_.get(), CURLOPT_SSL_VERIFYHOST, 2L));
        require(curl_easy_setopt(curl_.get(), CURLOPT_NOSIGNAL, 1L));
        require(curl_easy_setopt(
            curl_.get(), CURLOPT_WS_OPTIONS, static_cast<long>(CURLWS_NOAUTOPONG)));
        require(curl_easy_setopt(curl_.get(), CURLOPT_HTTPHEADER, headers_->get()));

        multi_ = curl_multi_init();
        if (!multi_) {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_connection_failed));
        }
        if (curl_multi_add_handle(multi_, curl_.get()) != CURLM_OK) {
            curl_multi_cleanup(multi_);
            multi_ = nullptr;
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_connection_failed));
        }
        attached_ = true;
        int running = 1;
        while (running) {
            if (cancelled()) {
                throw XaiVoiceFailure(
                    ErrorCode::operation_cancelled,
                    std::string(xai_operation_cancelled));
            }
            if (clock::now() >= deadline) {
                throw XaiVoiceFailure(
                    ErrorCode::command_timeout, std::string(xai_timed_out));
            }
            if (curl_multi_perform(multi_, &running) != CURLM_OK) {
                throw XaiVoiceFailure(
                    ErrorCode::internal_error, std::string(xai_connection_failed));
            }
            if (!running) break;
            if (curl_multi_poll(multi_, nullptr, 0, wait_budget_ms(deadline), nullptr)
                != CURLM_OK) {
                throw XaiVoiceFailure(
                    ErrorCode::internal_error, std::string(xai_connection_failed));
            }
        }
        int remaining = 0;
        const CURLMsg* message = curl_multi_info_read(multi_, &remaining);
        if (!message || message->msg != CURLMSG_DONE) {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_connection_failed));
        }
        long http_status = 0;
        curl_easy_getinfo(curl_.get(), CURLINFO_RESPONSE_CODE, &http_status);
        if (http_status == 401 || http_status == 403) {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_authentication_failed));
        }
        if (http_status == 400 || http_status == 404 || http_status == 422) {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_configuration_rejected));
        }
        if (message->data.result != CURLE_OK || (http_status != 0 && http_status != 101)) {
            log_warn(
                "xAI voice input connection failed (curl "
                + std::to_string(message->data.result) + ")");
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_connection_failed));
        }
        // The easy handle stays on this multi handle until close(). Removing it
        // after CONNECT_ONLY makes later websocket send and receive fail.
    }

    void send(
        std::string_view payload,
        bool binary,
        clock::time_point deadline,
        const std::function<bool()>& cancelled) override {
        if (!attached_ || payload.empty()) {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_connection_failed));
        }
        // A failed send leaves sending_ set, so no pong enters the unfinished
        // frame. The dictation ends and close() clears it.
        sending_ = true;
        const unsigned flags = binary ? CURLWS_BINARY : CURLWS_TEXT;
        std::size_t offset = 0;
        while (offset < payload.size()) {
            if (cancelled()) {
                throw XaiVoiceFailure(
                    ErrorCode::operation_cancelled,
                    std::string(xai_operation_cancelled));
            }
            if (clock::now() >= deadline) {
                throw XaiVoiceFailure(
                    ErrorCode::command_timeout, std::string(xai_timed_out));
            }
            std::size_t sent = 0;
            const CURLcode result = curl_ws_send(
                curl_.get(),
                payload.data() + offset,
                payload.size() - offset,
                &sent,
                0,
                flags);
            offset += sent;
            pump_readable();
            if (result == CURLE_OK && sent > 0) continue;
            if (result != CURLE_AGAIN) {
                log_warn(
                    "xAI voice input send failed (curl "
                    + std::to_string(result) + ")");
                throw XaiVoiceFailure(
                    ErrorCode::internal_error, std::string(xai_connection_failed));
            }
            wait_for_socket(wait_budget_ms(deadline), true);
        }
        sending_ = false;
        try_pong(deadline, cancelled);
    }

    std::optional<XaiIncoming> recv(std::chrono::milliseconds wait) override {
        if (!inbox_.empty()) {
            flush_idle_pong();
            XaiIncoming message = std::move(inbox_.front());
            inbox_.pop_front();
            return message;
        }
        if (!attached_) return std::nullopt;
        const auto deadline = clock::now() + wait;
        for (;;) {
            if (pump_readable()) {
                flush_idle_pong();
                if (!inbox_.empty()) {
                    XaiIncoming message = std::move(inbox_.front());
                    inbox_.pop_front();
                    return message;
                }
            }
            if (clock::now() >= deadline) return std::nullopt;
            wait_for_socket(wait_budget_ms(deadline), false);
        }
    }

    void close() override {
        if (attached_ && curl_.get()) {
            std::size_t sent = 0;
            curl_ws_send(curl_.get(), "", 0, &sent, 0, CURLWS_CLOSE);
            curl_multi_remove_handle(multi_, curl_.get());
            attached_ = false;
        }
        if (multi_) {
            curl_multi_cleanup(multi_);
            multi_ = nullptr;
        }
        curl_ = CurlHandle();
        headers_.reset();
        fragment_.clear();
        fragment_open_ = false;
        inbox_.clear();
        pong_payload_.clear();
        pong_waiting_ = false;
        sending_ = false;
    }

private:
    bool pump_readable() {
        if (!attached_) return false;
        bool produced = false;
        for (;;) {
            char buffer[4096];
            std::size_t received = 0;
            const curl_ws_frame* meta = nullptr;
            const CURLcode result = curl_ws_recv(
                curl_.get(), buffer, sizeof(buffer), &received, &meta);
            if (result == CURLE_AGAIN) return produced;
            if (result != CURLE_OK || !meta) {
                inbox_.push_back(XaiIncoming{.closed = true});
                return true;
            }
            consume(*meta, std::string_view(buffer, received));
            produced = true;
        }
    }

    void flush_idle_pong() {
        if (!pong_waiting_ || sending_) return;
        try_pong(clock::now() + std::chrono::milliseconds(100), [] { return false; });
    }

    void consume(const curl_ws_frame& meta, std::string_view chunk) {
        if (meta.flags & CURLWS_PING) {
            pong_payload_.assign(chunk);
            pong_waiting_ = true;
            return;
        }
        if (meta.flags & CURLWS_PONG) return;
        if (meta.flags & CURLWS_CLOSE) {
            fragment_.clear();
            fragment_open_ = false;
            inbox_.push_back(XaiIncoming{.closed = true});
            return;
        }
        const bool data = (meta.flags & (CURLWS_TEXT | CURLWS_BINARY)) != 0;
        if (!data) return;
        if (!fragment_open_) {
            fragment_.clear();
            fragment_binary_ = (meta.flags & CURLWS_BINARY) != 0;
            fragment_open_ = true;
        }
        if (fragment_.size() + chunk.size() > xai_max_provider_message) {
            throw XaiVoiceFailure(
                ErrorCode::internal_error, std::string(xai_malformed_transcript));
        }
        fragment_.append(chunk);
        if (meta.bytesleft > 0 || (meta.flags & CURLWS_CONT)) return;
        XaiIncoming message;
        message.binary = fragment_binary_;
        message.payload = std::move(fragment_);
        fragment_.clear();
        fragment_open_ = false;
        inbox_.push_back(std::move(message));
    }

    void try_pong(
        clock::time_point deadline,
        const std::function<bool()>& cancelled) {
        if (!pong_waiting_ || sending_) return;
        std::size_t offset = 0;
        while (pong_waiting_) {
            if (cancelled() || clock::now() >= deadline) return;
            std::size_t sent = 0;
            const char* data = pong_payload_.data() + offset;
            const std::size_t left = pong_payload_.size() - offset;
            const CURLcode result = curl_ws_send(
                curl_.get(), data, left, &sent, 0, CURLWS_PONG);
            if (sent > 0) offset += sent;
            if (result == CURLE_OK && offset >= pong_payload_.size()) {
                pong_waiting_ = false;
                pong_payload_.clear();
                return;
            }
            if (result != CURLE_OK && result != CURLE_AGAIN) {
                pong_waiting_ = false;
                return;
            }
            wait_for_socket(wait_budget_ms(deadline), true);
        }
    }

    // Wait for one direction only. The socket is almost always writable, so
    // a read wait that also asks for writable returns at once and spins.
    void wait_for_socket(int timeout_ms, bool writable) {
        if (!attached_) return;
        curl_socket_t sock = CURL_SOCKET_BAD;
        if (curl_easy_getinfo(curl_.get(), CURLINFO_ACTIVESOCKET, &sock) != CURLE_OK
            || sock == CURL_SOCKET_BAD) {
            return;
        }
#ifdef _WIN32
        WSAPOLLFD descriptor{};
        descriptor.fd = sock;
        descriptor.events = writable ? POLLWRNORM : POLLRDNORM;
        WSAPoll(&descriptor, 1, timeout_ms);
#else
        pollfd descriptor{};
        descriptor.fd = static_cast<int>(sock);
        descriptor.events = writable ? POLLOUT : POLLIN;
        poll(&descriptor, 1, timeout_ms);
#endif
    }

    CurlHandle curl_;
    std::unique_ptr<CurlHeaders> headers_;
    CURLM* multi_ = nullptr;
    bool attached_ = false;
    bool sending_ = false;
    std::string fragment_;
    bool fragment_open_ = false;
    bool fragment_binary_ = false;
    std::deque<XaiIncoming> inbox_;
    std::string pong_payload_;
    bool pong_waiting_ = false;
};

} // namespace

XaiVoiceFailure::XaiVoiceFailure(ErrorCode code, std::string message)
    : std::runtime_error(std::move(message)), code(code) {}

std::unique_ptr<XaiSocket> make_xai_curl_socket() {
    return std::make_unique<XaiCurlSocket>();
}

} // namespace cha::media
