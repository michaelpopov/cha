#include "ui/console/console_writer.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

namespace cha {
namespace {

TEST(ConsoleSurface, EmitsAttributesOnlyWhenColourIsEnabled) {
    std::ostringstream colored;
    ConsoleSurface colored_surface(colored, true);
    colored_surface.attributes(TranscriptAttributes::bold);
    colored_surface.attributes(TranscriptAttributes::dim);
    colored_surface.attributes(TranscriptAttributes::bold_dim);
    colored_surface.attributes(TranscriptAttributes::normal);
    EXPECT_EQ(
        colored.str(),
        "\x1b[1m\x1b[2m\x1b[1;2m\x1b[0m");

    std::ostringstream plain;
    ConsoleSurface plain_surface(plain, false);
    plain_surface.attributes(TranscriptAttributes::bold);
    plain_surface.attributes(TranscriptAttributes::normal);
    EXPECT_TRUE(plain.str().empty());
}

TEST(ConsoleSurface, NeutralizesTerminalControlsInBothColourModes) {
    const std::string hostile =
        "\x1b]0;title\x07 \x1b[31m \x1b]52;c;data\x07\r\n\t";
    for (const bool color : {false, true}) {
        std::ostringstream output;
        ConsoleSurface surface(output, color);
        surface.write(hostile);
        EXPECT_EQ(output.str().find('\x1b'), std::string::npos);
        EXPECT_EQ(output.str().find('\x07'), std::string::npos);
        EXPECT_EQ(output.str().find('\r'), std::string::npos);
        EXPECT_NE(output.str().find("^["), std::string::npos);
        EXPECT_NE(output.str().find("^G"), std::string::npos);
        EXPECT_NE(output.str().find("\n\t"), std::string::npos);
    }
}

TEST(ConsoleSurface, PreservesUtf8AndNeutralizesC1Controls) {
    const std::string utf8 = "caf\xc3\xa9\t\n";
    EXPECT_EQ(sanitize_console_text(utf8), utf8);
    const std::string csi = "before\xc2\x9b" "31mafter";
    const std::string sanitized = sanitize_console_text(csi);
    EXPECT_EQ(sanitized.find("\xc2\x9b"), std::string::npos);
    EXPECT_NE(sanitized.find("[C1]"), std::string::npos);
}

} // namespace
} // namespace cha
