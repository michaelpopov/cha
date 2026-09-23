#pragma once

#include "runtime/protocol.h"
#include "session/submission.h"
#include "util/wake_notifier.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

namespace cha {

struct CommandFailure {
    ErrorCode code;
    std::string message;
};

using CommandSubmitResult = std::variant<
    CommandResult,
    SessionSnapshot,
    SessionLabelResult,
    SubscribeResult,
    CommandFailure,
    ErrorCode>;

class CommandReply {
public:
    explicit CommandReply(std::shared_ptr<SubmissionState> submission = std::make_shared<SubmissionState>(),
        std::shared_ptr<WakeNotifier> notifier = {})
        : submission_(std::move(submission)), notifier_(std::move(notifier)) {}
    std::shared_ptr<SubmissionState> submission() const { return submission_; }
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
    std::shared_ptr<SubmissionState> submission_;
    std::shared_ptr<WakeNotifier> notifier_;
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

} // namespace cha
