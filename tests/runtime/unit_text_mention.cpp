#include "runtime/text_input.h"

#include <gtest/gtest.h>

namespace cha {

TEST(Mention, ParsesAddressingAndPreservesLiteralText) {
    struct Case {
        const char* input;
        const char* handle;
        const char* text;
    };
    const Case cases[]{
        {"   ordinary", "", "   ordinary"},
        {"  @Ada hello", "Ada", "hello"},
        {"  @@Ada hello", "", "  @Ada hello"},
        {"@", "", "@"},
        {"@ hello", "", "@ hello"},
        {"   ", "", "   "},
        {"@Ada, hello\nworld", "Ada,", "hello\nworld"},
        {"@Иван привет", "Иван", "привет"},
        {"@Ada", "Ada", ""},
        {"  @Ada   ", "Ada", ""},
        {"@A @B do this", "A", "@B do this"},
        {"@Ada /clear", "Ada", "/clear"},
        {"plain   ", "", "plain   "},
        {"\n  ", "", "\n  "},
        {"@@", "", "@"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.input);
        const AddressedPrompt parsed = parse_addressed_prompt(item.input);
        EXPECT_EQ(parsed.handle, item.handle);
        EXPECT_EQ(parsed.text, item.text);
    }
}

} // namespace cha
