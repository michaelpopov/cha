#include "util/input_wait.h"

#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

namespace cha {
namespace {

TEST(InputEvents, ReportsClosedStdinFromPoll) {
    int pipe_descriptors[2]{};
    ASSERT_EQ(::pipe(pipe_descriptors), 0);
    const int saved_stdin = ::dup(STDIN_FILENO);
    ASSERT_NE(saved_stdin, -1);
    const int notification_descriptor =
        ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_NE(notification_descriptor, -1);
    ASSERT_NE(::dup2(pipe_descriptors[0], STDIN_FILENO), -1);
    ::close(pipe_descriptors[0]);
    ::close(pipe_descriptors[1]);

    const InputEvents ready =
        wait_for_input_events(notification_descriptor);

    const int restore_result = ::dup2(saved_stdin, STDIN_FILENO);
    ::close(saved_stdin);
    ::close(notification_descriptor);
    ASSERT_NE(restore_result, -1);
    EXPECT_TRUE(ready.input_closed());
    EXPECT_FALSE(ready.failed());
    EXPECT_FALSE(ready.signal_ready());
}

TEST(InputEvents, ReportsAReadableSignalDescriptor) {
    const int notification_descriptor =
        ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_NE(notification_descriptor, -1);
    const int signal_descriptor =
        ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_NE(signal_descriptor, -1);
    ASSERT_EQ(::eventfd_write(signal_descriptor, 1), 0);

    const InputEvents ready =
        wait_for_input_events(notification_descriptor, signal_descriptor);

    ::close(signal_descriptor);
    ::close(notification_descriptor);
    EXPECT_TRUE(ready.signal_ready());
    EXPECT_FALSE(ready.notification_ready());
}

} // namespace
} // namespace cha
