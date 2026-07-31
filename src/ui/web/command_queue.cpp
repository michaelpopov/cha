#include "ui/web/command_queue.h"

#include <utility>

namespace cha::web {

void CommandCompletion::complete(CommandSubmitResult result) {
    {
        std::lock_guard lock(mutex_);
        if (result_) {
            return;
        }
        result_ = std::move(result);
    }
    ready_.notify_all();
}

std::optional<CommandSubmitResult> CommandCompletion::wait_for(
    std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    if (!ready_.wait_for(lock, timeout, [this] { return result_.has_value(); })) {
        return std::nullopt;
    }
    return result_;
}

CommandEnqueueResult CommandQueue::try_push(OwnerCommand command) {
    std::lock_guard lock(mutex_);
    if (commands_.size() == capacity_) {
        return {};
    }
    const bool wake_owner = commands_.empty();
    commands_.push_back(std::move(command));
    return {
        .accepted = true,
        .wake_owner = wake_owner,
    };
}

std::optional<OwnerCommand> CommandQueue::try_pop() {
    std::lock_guard lock(mutex_);
    if (commands_.empty()) {
        return std::nullopt;
    }
    OwnerCommand command = std::move(commands_.front());
    commands_.pop_front();
    return command;
}

} // namespace cha::web
