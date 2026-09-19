#pragma once

#include "web/protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

namespace cha::web {

using CommandSubmitResult = std::variant<
    CommandResult,
    SessionSnapshot,
    SessionLabelResult,
    SubscribeResult,
    ErrorCode>;

class CommandReply {
public:
    // Returns false when the sole waiter has already timed out or another
    // result won the reply race.
    [[nodiscard]] bool complete(CommandSubmitResult result);
    [[nodiscard]] std::optional<CommandSubmitResult> wait_for(
        std::chrono::milliseconds timeout) const;
    // Native completion: invoked once when a result is stored. Already-complete
    // replies run the callback immediately. Abandoned replies ignore it.
    void set_ready_callback(std::function<void()> callback);
    [[nodiscard]] std::optional<CommandSubmitResult> peek() const;
    void abandon() const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable ready_;
    mutable bool abandoned_{};
    std::optional<CommandSubmitResult> result_;
    mutable std::function<void()> ready_callback_;
};

struct OwnerCommand {
    WebCommand command;
    std::shared_ptr<CommandReply> reply;
    // Owner-local subscribe generation; not a wire field.
    std::uint64_t subscribe_ticket{};
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
