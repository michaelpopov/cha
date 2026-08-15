#include "web/session_markdown.h"

#include <gtest/gtest.h>

namespace cha::web {
namespace {

TEST(SessionMarkdown, ExportsSpeakersAndPreservesEntryMarkdown) {
    const std::vector<TranscriptEntry> entries{
        make_human_entry({
            .id = 1,
            .author = {"reader", "Reader"},
            .addressed_to = {"guide", "Guide"},
            .text = "Can you review **this**?",
            .request_id = 1,
        }),
        make_character_entry(
            2, "guide", "Guide #1", "- First\n- Second", EntryStatus::complete, 1),
    };

    EXPECT_EQ(session_markdown("Plan #1", entries),
        "# Plan \\#1\n"
        "\n## Reader\n\n"
        "Can you review **this**?\n"
        "\n## Guide \\#1\n\n"
        "- First\n- Second\n");
}

TEST(SessionMarkdown, ExportsAnEmptySessionAsItsTitle) {
    EXPECT_EQ(session_markdown("Notes", {}), "# Notes\n");
}

TEST(SessionMarkdown, OmitsTransientOffRecordMarkers) {
    const std::vector<TranscriptEntry> entries{
        make_hide_on_marker(1),
        make_character_entry(
            2, "guide", "Guide", "Off the record.", EntryStatus::complete, 1),
        make_hide_off_marker(3),
    };

    EXPECT_EQ(session_markdown("Plan", entries),
        "# Plan\n"
        "\n## Guide\n\n"
        "Off the record.\n");
}

} // namespace
} // namespace cha::web
