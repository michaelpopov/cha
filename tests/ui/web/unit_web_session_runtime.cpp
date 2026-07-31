#include "ui/web/web_session_runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

struct FakeState {
    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable release;
    std::thread::id owner_id;
    std::vector<std::string> raw_inputs;
    std::vector<std::string> default_ids;
    std::vector<std::size_t> raw_counts_at_receive;
    std::vector<int> receives_at_raw_input;
    int stops{};
    int receives{};
    bool block_raw{};
    bool raw_entered{};
    bool released{};
    bool block_receive{};
    bool receive_entered{};
    bool receive_released{};
    bool flood_notifications{};
    bool end_on_receive{};
    bool shutdown{};
};

class FakeController final : public WebSessionController {
public:
    FakeController(
        std::shared_ptr<FakeState> state,
        WakeNotifier&)
        : state_(std::move(state)) {}

    SessionUpdate handle_raw_input(std::string input) override {
        std::unique_lock lock(state_->mutex);
        check_owner();
        state_->raw_inputs.push_back(std::move(input));
        state_->receives_at_raw_input.push_back(state_->receives);
        state_->raw_entered = true;
        state_->entered.notify_all();
        state_->release.wait(lock, [this] {
            return !state_->block_raw || state_->released;
        });
        return {
            .end_session = state_->raw_inputs.back() == "end session",
            .clear_input = true,
            .notice = "raw",
        };
    }
    SessionUpdate request_stop() override {
        std::lock_guard lock(state_->mutex);
        check_owner();
        ++state_->stops;
        return {.notice = "stopped"};
    }
    SessionUpdate set_default_agent_id(std::string_view id) override {
        std::lock_guard lock(state_->mutex);
        check_owner();
        state_->default_ids.emplace_back(id);
        return {.notice = "default"};
    }
    SessionEventBatch receive(std::size_t) override {
        bool full = false;
        bool end_session = false;
        {
            std::unique_lock lock(state_->mutex);
            check_owner();
            ++state_->receives;
            state_->raw_counts_at_receive.push_back(
                state_->raw_inputs.size());
            state_->receive_entered = true;
            state_->entered.notify_all();
            state_->release.wait(lock, [this] {
                return !state_->block_receive || state_->receive_released;
            });
            full = state_->flood_notifications;
            end_session = state_->end_on_receive;
            state_->end_on_receive = false;
        }
        if (full) {
            return {
                .update = {
                    .render_needed = true,
                    .end_session = end_session,
                    .notice = "event",
                },
                .full = true,
            };
        }
        return {.update = {.end_session = end_session}};
    }
    void shutdown() override {
        std::lock_guard lock(state_->mutex);
        check_owner();
        state_->shutdown = true;
        state_->entered.notify_all();
    }

private:
    void check_owner() {
        if (state_->owner_id == std::thread::id{}) {
            state_->owner_id = std::this_thread::get_id();
        }
        EXPECT_EQ(state_->owner_id, std::this_thread::get_id());
    }
    std::shared_ptr<FakeState> state_;
};

WebControllerFactory fake_factory(const std::shared_ptr<FakeState>& state) {
    return [state](WakeNotifier& notifier) {
        return std::make_unique<FakeController>(state, notifier);
    };
}

WebSettings test_settings(
    std::size_t queue_capacity,
    std::size_t command_batch_size = 8,
    std::size_t event_batch_size = 8) {
    WebSettings settings;
    settings.command_queue_capacity = queue_capacity;
    settings.command_batch_size = command_batch_size;
    settings.event_batch_size = event_batch_size;
    return settings;
}

TEST(WakeNotifier, RemembersWakeBeforeWait) {
    WakeNotifier notifier;
    notifier.wake();

    EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 50ms));
}

TEST(WakeNotifier, CoalescesMultipleWakes) {
    WakeNotifier notifier;
    notifier.wake();
    notifier.wake();

    EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 50ms));
    EXPECT_FALSE(notifier.wait_until(std::chrono::steady_clock::now() + 5ms));
}

TEST(WakeNotifier, ReturnsFalseAtDeadlineWithoutWake) {
    WakeNotifier notifier;

    EXPECT_FALSE(notifier.wait_until(std::chrono::steady_clock::now() + 5ms));
}

TEST(CommandCompletion, FirstCompletionWins) {
    CommandCompletion completion;
    completion.complete(CommandResult{.notice = "first"});
    completion.complete(ErrorCode::internal_error);

    const auto result = completion.wait_for(1ms);
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<CommandResult>(*result).notice, "first");
}

TEST(WebSessionRuntime, RoutesRawAndTypedCommandsOnOneOwnerThread) {
    auto state = std::make_shared<FakeState>();
    WebSessionRuntime runtime(fake_factory(state), test_settings(8));

    EXPECT_EQ(
        std::get<CommandResult>(runtime.submit(RawCommand{"/clear"}, 1s))
            .notice,
        "raw");
    EXPECT_EQ(
        std::get<CommandResult>(runtime.submit(StopCommand{}, 1s)).notice,
        "stopped");
    const CommandResult default_changed =
        std::get<CommandResult>(runtime.submit(
            SetDefaultAgentCommand{"stable-id"},
            1s));
    EXPECT_EQ(default_changed.notice, "default");
    EXPECT_FALSE(default_changed.clear_input);

    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->raw_inputs, std::vector<std::string>({"/clear"}));
    EXPECT_EQ(state->stops, 1);
    EXPECT_EQ(state->default_ids, std::vector<std::string>({"stable-id"}));
    EXPECT_NE(state->owner_id, std::this_thread::get_id());
}

TEST(WebSessionRuntime, TimeoutLeavesAcceptedCommandAliveAndLateCompletionSafe) {
    auto state = std::make_shared<FakeState>();
    state->block_raw = true;
    WebSessionRuntime runtime(fake_factory(state), test_settings(2));

    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(RawCommand{"slow"}, 5ms)),
        ErrorCode::command_timeout);
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->raw_entered;
        }));
        state->released = true;
    }
    state->release.notify_all();
    EXPECT_EQ(
        std::get<CommandResult>(runtime.submit(StopCommand{}, 1s)).notice,
        "stopped");
}

TEST(WebSessionRuntime, FullAndStoppingCommandsDoNotExecute) {
    auto state = std::make_shared<FakeState>();
    state->block_raw = true;
    WebSessionRuntime runtime(fake_factory(state), test_settings(1));
    auto first = std::async(std::launch::async, [&] {
        return runtime.submit(RawCommand{"first"}, 1s);
    });
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->raw_entered;
        }));
    }
    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(RawCommand{"second"}, 5ms)),
        ErrorCode::command_timeout);
    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(StopCommand{}, 1s)),
        ErrorCode::command_queue_full);
    runtime.request_shutdown();
    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(StopCommand{}, 1s)),
        ErrorCode::session_not_live);
    {
        std::lock_guard lock(state->mutex);
        state->released = true;
    }
    state->release.notify_all();
    (void)first.get();
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->stops, 0);
    EXPECT_EQ(state->raw_inputs, std::vector<std::string>({"first"}));
}

TEST(WebSessionRuntime, ContinuesAfterAFullBatchWithOnlyACoalescedWake) {
    auto state = std::make_shared<FakeState>();
    state->block_receive = true;
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(4, 2));
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->receive_entered;
        }));
    }
    // Only the first push into the empty queue wakes the owner. It observes
    // that wake before starting a batch, leaving no later wake to mask a
    // missing full-batch continuation.
    for (int index = 0; index != 3; ++index) {
        EXPECT_EQ(
            std::get<ErrorCode>(runtime.submit(RawCommand{"queued"}, 1ms)),
            ErrorCode::command_timeout);
    }
    {
        std::lock_guard lock(state->mutex);
        state->receive_released = true;
    }
    state->release.notify_all();

    std::unique_lock lock(state->mutex);
    EXPECT_TRUE(state->entered.wait_for(lock, 500ms, [&] {
        return state->raw_inputs.size() == 3U;
    }));
    EXPECT_GE(state->receives, 2);
}

TEST(WebSessionRuntime, IndependentRuntimesProgressWithoutSharedState) {
    auto first_state = std::make_shared<FakeState>();
    auto second_state = std::make_shared<FakeState>();
    WebSessionRuntime first(fake_factory(first_state), test_settings(4));
    WebSessionRuntime second(fake_factory(second_state), test_settings(4));

    auto first_result = std::async(std::launch::async, [&] {
        return first.submit(RawCommand{"first"}, 1s);
    });
    auto second_result = std::async(std::launch::async, [&] {
        return second.submit(RawCommand{"second"}, 1s);
    });

    EXPECT_TRUE(std::holds_alternative<CommandResult>(first_result.get()));
    EXPECT_TRUE(std::holds_alternative<CommandResult>(second_result.get()));
    std::scoped_lock lock(first_state->mutex, second_state->mutex);
    EXPECT_EQ(first_state->raw_inputs, std::vector<std::string>({"first"}));
    EXPECT_EQ(second_state->raw_inputs, std::vector<std::string>({"second"}));
    EXPECT_NE(first_state->owner_id, second_state->owner_id);
}

TEST(WebSessionRuntime, InterleavesAgentDrainingWithCommandBatches) {
    auto state = std::make_shared<FakeState>();
    state->block_receive = true;
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(8, 2));
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->receive_entered;
        }));
    }
    for (int index = 0; index != 5; ++index) {
        EXPECT_EQ(
            std::get<ErrorCode>(runtime.submit(RawCommand{"/info"}, 1ms)),
            ErrorCode::command_timeout);
    }
    {
        std::lock_guard lock(state->mutex);
        state->receive_released = true;
    }
    state->release.notify_all();

    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->entered.wait_for(lock, 500ms, [&] {
        return state->raw_inputs.size() == 5U;
    }));
    EXPECT_NE(
        std::find(
            state->raw_counts_at_receive.begin(),
            state->raw_counts_at_receive.end(),
            2U),
        state->raw_counts_at_receive.end());
    EXPECT_NE(
        std::find(
            state->raw_counts_at_receive.begin(),
            state->raw_counts_at_receive.end(),
            4U),
        state->raw_counts_at_receive.end());
}

TEST(WebSessionRuntime, NotificationPressureDoesNotStarveCommands) {
    auto state = std::make_shared<FakeState>();
    state->flood_notifications = true;
    WebSessionRuntime runtime(fake_factory(state), test_settings(8));

    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->receives >= 8;
        }));
    }

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"during notification pressure"}, 1s)));

    std::lock_guard lock(state->mutex);
    EXPECT_TRUE(state->flood_notifications);
    ASSERT_EQ(state->receives_at_raw_input.size(), 1U);
    EXPECT_GE(state->receives_at_raw_input.front(), 8);
    EXPECT_EQ(
        state->raw_inputs,
        std::vector<std::string>({"during notification pressure"}));
}

TEST(WebSessionRuntime, CommandEndSessionStopsTheRuntime) {
    auto state = std::make_shared<FakeState>();
    WebSessionRuntime runtime(fake_factory(state), test_settings(2));

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"end session"}, 1s)));
    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(StopCommand{}, 1s)),
        ErrorCode::session_not_live);
}

TEST(WebSessionRuntime, ReceiveEndSessionStopsTheRuntime) {
    auto state = std::make_shared<FakeState>();
    state->end_on_receive = true;
    WebSessionRuntime runtime(fake_factory(state), test_settings(2));
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->shutdown;
        }));
    }

    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(StopCommand{}, 1s)),
        ErrorCode::session_not_live);
}

TEST(WebSessionRuntime, RejectsZeroBatchSizesBeforeStartingOwner) {
    WebSettings settings = test_settings(2);
    settings.command_batch_size = 0;

    EXPECT_THROW(
        (void)WebSessionRuntime(fake_factory(std::make_shared<FakeState>()), settings),
        std::invalid_argument);
}

} // namespace
} // namespace cha::web
