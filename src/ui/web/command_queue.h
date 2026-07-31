#pragma once

#include "ui/web/protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

namespace cha::web {

using CommandSubmitResult = std::variant<CommandResult, ErrorCode>;

class CommandCompletion {
public:
    void complete(CommandSubmitResult result);
    [[nodiscard]] std::optional<CommandSubmitResult> wait_for(
        std::chrono::milliseconds timeout) const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable ready_;
    std::optional<CommandSubmitResult> result_;
};

struct SseConnectNotification { std::uint64_t connection_id{}; };
struct SseDisconnectNotification { std::uint64_t connection_id{}; };
using OwnerNotification =
    std::variant<SseConnectNotification, SseDisconnectNotification>;

struct OwnerCommand {
    WebCommand command;
    std::shared_ptr<CommandCompletion> completion;
};

struct CommandEnqueueResult {
    bool accepted{};
    bool wake_owner{};
};

class CommandQueue {
public:
    explicit CommandQueue(std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] CommandEnqueueResult try_push(OwnerCommand command);
    [[nodiscard]] std::optional<OwnerCommand> try_pop();

private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::deque<OwnerCommand> commands_;
};

} // namespace cha::web
