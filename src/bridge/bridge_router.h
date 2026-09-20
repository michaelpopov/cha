#pragma once

#include "app/application.h"
#include "bridge/bridge_protocol.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace cha::bridge {

class BridgeRouter {
public:
    struct Options {
        std::string platform{"test"};
        std::size_t ordinary_limit{16};
        std::size_t control_limit{8};
        std::size_t request_bytes_limit{65536};
        std::optional<std::chrono::milliseconds> command_deadline;
        std::function<std::chrono::steady_clock::time_point()> clock;
    };

    explicit BridgeRouter(app::Application& application);
    BridgeRouter(app::Application& application, Options options);
    ~BridgeRouter();
    BridgeRouter(const BridgeRouter&) = delete;
    BridgeRouter& operator=(const BridgeRouter&) = delete;

    // Native-owned identity. A matching JSON connection_id does not create it.
    [[nodiscard]] std::string open_connection();
    void close_connection(std::string_view connection_id);

    void handle_request(
        std::string_view trusted_connection_id,
        std::string_view json);
    void handle_ack(
        std::string_view trusted_connection_id,
        std::string_view json);

    void run_tasks();
    void run_control_tasks();
    void run_ordinary_tasks();
    void expire_timeouts();
    void pump_output();
    [[nodiscard]] std::optional<nlohmann::json> take_delivery(
        std::string_view connection_id);
    bool wait_for_work(std::chrono::milliseconds timeout);
    bool wait_for_ordinary_work(std::chrono::milliseconds timeout);
    void shutdown();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace cha::bridge
