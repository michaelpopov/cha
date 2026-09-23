#pragma once

#include "app/application.h"
#include "app/background_jobs.h"
#include "media/xai_socket.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cha::media {

struct XaiDeadlineBudget {
    std::chrono::milliseconds startup{0};
    std::chrono::milliseconds stop_budget{0};
    std::chrono::milliseconds audio_send{0};
};

[[nodiscard]] XaiDeadlineBudget xai_deadline_budget(
    std::chrono::milliseconds deadline);

[[nodiscard]] std::chrono::milliseconds xai_stop_limit(
    std::chrono::milliseconds remaining,
    std::chrono::milliseconds deadline);

class XaiVoiceSessions {
public:
    explicit XaiVoiceSessions(app::BackgroundJobs& jobs);
    ~XaiVoiceSessions();

    XaiVoiceSessions(const XaiVoiceSessions&) = delete;
    XaiVoiceSessions& operator=(const XaiVoiceSessions&) = delete;

    using SocketFactory = std::function<std::unique_ptr<XaiSocket>()>;
    void set_socket_factory(SocketFactory factory);

    [[nodiscard]] std::shared_ptr<app::OperationReply> start(
        std::string connection_id,
        std::uint64_t request_id,
        std::string session_id,
        std::uint64_t epoch,
        std::string url,
        std::string authorization,
        std::chrono::milliseconds deadline);

    [[nodiscard]] std::shared_ptr<app::OperationReply> audio(
        std::string connection_id,
        std::uint64_t request_id,
        std::string session_id,
        std::vector<unsigned char> pcm,
        std::chrono::milliseconds deadline);

    [[nodiscard]] std::shared_ptr<app::OperationReply> stop(
        std::string connection_id,
        std::uint64_t request_id,
        std::string session_id,
        std::chrono::milliseconds remaining,
        std::chrono::milliseconds deadline);

    void cancel(std::string_view connection_id, std::string_view session_id);
    void cancel_connection(std::string_view connection_id);
    void cancel_all();
    void expire(std::string_view connection_id, std::uint64_t request_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha::media
