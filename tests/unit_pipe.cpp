#include "pipe.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <poll.h>
#include <string>
#include <thread>

namespace cha {
namespace {

PipeEvent data(std::string value) {
    return {PipeEventKind::data, std::move(value)};
}

TEST(Pipe, DeliversMessagesInOrder) {
    Pipe pipe;
    pipe.put("first");
    pipe.put("second");

    EXPECT_EQ(pipe.get(), data("first"));
    EXPECT_EQ(pipe.get(), data("second"));
}

TEST(Pipe, EmitsEndOfMessageMarker) {
    Pipe pipe;
    pipe.eom();

    EXPECT_EQ(pipe.get(), (PipeEvent{PipeEventKind::eom, {}}));
}

TEST(Pipe, DeliversConversationAndModelEventsWithoutMessageContent) {
    Pipe pipe;
    pipe.conversation_updated();
    pipe.model_changed("new-model");

    EXPECT_EQ(pipe.get(), (PipeEvent{PipeEventKind::conversation_updated, {}}));
    EXPECT_EQ(pipe.get(), (PipeEvent{PipeEventKind::model_changed, "new-model"}));
}

TEST(Pipe, CloseUnblocksAWaitingReader) {
    Pipe pipe;
    std::promise<PipeEvent> result_promise;
    auto result = result_promise.get_future();
    std::thread reader([&pipe, &result_promise] {
        result_promise.set_value(pipe.get());
    });

    pipe.close();

    EXPECT_EQ(result.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_EQ(result.get(), (PipeEvent{PipeEventKind::closed, {}}));
    reader.join();
}

TEST(Pipe, TryGetUsesPollableNotifications) {
    Pipe pipe;
    pollfd descriptor{pipe.notification_fd(), POLLIN, 0};
    EXPECT_EQ(::poll(&descriptor, 1, 0), 0);

    pipe.put("ready");
    descriptor.revents = 0;
    ASSERT_EQ(::poll(&descriptor, 1, 100), 1);
    EXPECT_NE(descriptor.revents & POLLIN, 0);

    PipeEvent event{PipeEventKind::eom, {}};
    ASSERT_TRUE(pipe.try_get(event));
    EXPECT_EQ(event, data("ready"));
    EXPECT_FALSE(pipe.try_get(event));

    descriptor.revents = 0;
    EXPECT_EQ(::poll(&descriptor, 1, 0), 0);
}

} // namespace
} // namespace cha
