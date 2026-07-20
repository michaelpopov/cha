#include "command.h"
#include "input_editor.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(Command, ParsesCommands) {
    EXPECT_EQ(parse_command("/stop").kind, CommandKind::stop);
    EXPECT_EQ(parse_command("Hello").kind, CommandKind::text);
    EXPECT_EQ(parse_command("/unknown").kind, CommandKind::unknown);
    EXPECT_EQ(parse_command("/model other-model").kind, CommandKind::unknown);
}

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

} // namespace
} // namespace cha
