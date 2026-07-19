#include "command.h"
#include "input_editor.h"
#include "text_layout.h"
#include "transcript.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(Command, ParsesCommandsAndUsesServerModelConfirmation) {
    const Command model = parse_command(".model\t replacement-model ");
    EXPECT_EQ(model.kind, CommandKind::model);
    EXPECT_EQ(model.argument, "replacement-model");
    EXPECT_EQ(parse_command(".stop").kind, CommandKind::stop);
    EXPECT_EQ(parse_command("Hello").kind, CommandKind::text);
    EXPECT_EQ(parse_command(".unknown").kind, CommandKind::unknown);
    EXPECT_EQ(confirmed_model("Model: replacement-model"), std::optional<std::string>("replacement-model"));
    EXPECT_EQ(confirmed_model("Usage: .model MODEL"), std::nullopt);
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

TEST(Transcript, AccumulatesStreamingAssistantFragments) {
    Transcript transcript;
    EXPECT_EQ(transcript.revision(), 0U);
    transcript.add_user("Hello");
    transcript.begin_assistant();
    transcript.append_assistant("Hello");
    transcript.append_assistant(" there");
    const std::size_t rendered_revision = transcript.revision();
    transcript.finish_assistant();

    ASSERT_EQ(transcript.entries().size(), 2U);
    EXPECT_EQ(transcript.entries()[1].speaker, Speaker::assistant);
    EXPECT_EQ(transcript.entries()[1].text, "Hello there");
    EXPECT_EQ(rendered_revision, 4U);
    EXPECT_EQ(transcript.revision(), rendered_revision);
}

TEST(TextLayout, CountsEveryExplicitTranscriptLine) {
    std::string response;
    for (int line = 0; line < 100; ++line) {
        response += "item\n";
    }

    EXPECT_GE(text_layout::rows(response, 80), 101);
}

TEST(TextLayout, CountsWrappedAndMultilineWideInput) {
    const std::wstring input = L"first line\nsecond line\n1234567890";

    EXPECT_GE(text_layout::rows(input, 8, 2), 6);
}

} // namespace
} // namespace cha
