#include "web/session_markdown.h"

#include <gtest/gtest.h>

#include <regex>
#include <utility>

namespace cha::web {
namespace {

TEST(SessionMarkdown, ExportsSpeakersAndPreservesEntryMarkdown) {
    TranscriptEntry prompt = make_human_entry({
            .id = 1,
            .author = {"reader", "Reader"},
            .addressed_to = {"guide", "Guide"},
            .text = "Can you review **this**?",
            .request_id = 1,
            .created_at = 0,
        });
    TranscriptEntry response = make_character_entry(
        2, "guide", "Guide #1", "- First\n- Second", EntryStatus::complete, 1);
    response.created_at = 0;
    const std::vector<TranscriptEntry> entries{std::move(prompt), std::move(response)};

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

TEST(SessionMarkdown, ExportsKnownTimestampsInLocalTime) {
    const std::vector<TranscriptEntry> entries{
        {
            .id = 1,
            .kind = EntryKind::human,
            .participant_id = "reader",
            .display_name = "Reader",
            .addressed_to = "guide",
            .addressed_to_name = "Guide",
            .text = "When was this?",
            .created_at = 1'700'000'000,
        },
    };

    const std::string markdown = session_markdown("Plan", entries);
    EXPECT_TRUE(std::regex_search(markdown, std::regex(
        R"(## Reader\n\*[A-Z][a-z]+ [0-9]{2}, [0-9]{4}, [0-9]{2}:[0-9]{2} [A-Z]+\*\n\nWhen was this\?\n)")));
}

TEST(SessionMarkdown, OmitsTransientOffRecordMarkers) {
    TranscriptEntry response = make_character_entry(
        2, "guide", "Guide", "Off the record.", EntryStatus::complete, 1);
    response.created_at = 0;
    const std::vector<TranscriptEntry> entries{
        make_hide_on_marker(1),
        std::move(response),
        make_hide_off_marker(3),
    };

    EXPECT_EQ(session_markdown("Plan", entries),
        "# Plan\n"
        "\n## Guide\n\n"
        "Off the record.\n");
}

} // namespace
} // namespace cha::web
