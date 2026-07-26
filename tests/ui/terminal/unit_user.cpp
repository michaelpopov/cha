#include "ui/tui/input_editor.h"
#include "ui/tui/user.h"

#include <gtest/gtest.h>

#include <sys/eventfd.h>
#include <unistd.h>

namespace cha {
namespace {

TEST(InputEditor, EditsAndEncodesUnicodeInput) {
    InputEditor editor;
    editor.insert(L'H');
    editor.insert(L'\u00e9');
    editor.move_left();
    editor.insert(L'i');

    EXPECT_EQ(editor.value(), "Hi\xc3\xa9");
}

TEST(InputEditor, ConcatenatesContinuedLines) {
    InputEditor editor;
    for (const wchar_t character : std::wstring(L"first\\")) {
        editor.insert(character);
    }
    ASSERT_TRUE(editor.ends_with_continuation());
    editor.continue_line();
    for (const wchar_t character : std::wstring(L"second")) {
        editor.insert(character);
    }

    EXPECT_EQ(editor.value(), "firstsecond");
}

TEST(UserEvents, ReportsClosedStdinFromPoll) {
    int pipe_descriptors[2]{};
    ASSERT_EQ(::pipe(pipe_descriptors), 0);
    const int saved_stdin = ::dup(STDIN_FILENO);
    ASSERT_NE(saved_stdin, -1);
    const int agent_descriptor =
        ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    ASSERT_NE(agent_descriptor, -1);
    ASSERT_NE(::dup2(pipe_descriptors[0], STDIN_FILENO), -1);
    ::close(pipe_descriptors[0]);
    ::close(pipe_descriptors[1]);

    const UserEvents ready =
        wait_for_user_events(agent_descriptor);

    const int restore_result = ::dup2(saved_stdin, STDIN_FILENO);
    ::close(saved_stdin);
    ::close(agent_descriptor);
    ASSERT_NE(restore_result, -1);
    EXPECT_TRUE(ready.terminal_closed());
    EXPECT_FALSE(ready.failed());
}

} // namespace
} // namespace cha
