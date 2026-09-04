#include "web/session_markdown.h"

#include <gtest/gtest.h>

#include <regex>
#include <utility>

namespace cha::web {
namespace {

TEST(SessionMarkdown, ExportsSpeakerBadgesAndInlineMarkdown) {
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
        "<!-- CHA session: Plan #1 -->\n"
        "\n`Reader` · Can you review **this**?\n"
        "\n`Guide #1` · - First\n- Second\n");
}

TEST(SessionMarkdown, ExportsAnEmptySessionAsItsTitle) {
    EXPECT_EQ(session_markdown("Notes", {}), "<!-- CHA session: Notes -->\n");
}

TEST(SessionMarkdown, ExportsAMulticastPromptOnceAndEveryResponse) {
    TranscriptEntry first_prompt = make_human_entry({
        .id = 1,
        .author = {"reader", "Reader"},
        .addressed_to = {"one", "One"},
        .text = "Shared question",
        .request_id = 10,
        .created_at = 0,
    });
    TranscriptEntry first_response = make_character_entry(
        2, "one", "One", "One answer", EntryStatus::complete, 10);
    first_response.created_at = 0;
    TranscriptEntry second_prompt = make_human_entry({
        .id = 3,
        .author = {"reader", "Reader"},
        .addressed_to = {"two", "Two"},
        .text = "Shared question",
        .request_id = 11,
        .created_at = 0,
    });
    TranscriptEntry second_response = make_character_entry(
        4, "two", "Two", "Two answer", EntryStatus::complete, 11);
    second_response.created_at = 0;
    const std::vector<TranscriptEntry> entries{
        std::move(first_prompt),
        std::move(first_response),
        std::move(second_prompt),
        std::move(second_response),
    };

    EXPECT_EQ(session_markdown("Discussion", entries),
        "<!-- CHA session: Discussion -->\n"
        "\n`Reader` · Shared question\n"
        "\n`One` · One answer\n"
        "\n`Two` · Two answer\n");
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
        R"(\*Started [A-Z][a-z]+ [0-9]{2}, [0-9]{4} at [0-9]{2}:[0-9]{2} [A-Z]+\*\n\n`Reader` · When was this\?\n)")));
}

TEST(SessionMarkdown, CompactsParagraphsWithinOneMessage) {
    TranscriptEntry response = make_character_entry(
        1, "guide", "Guide", "First paragraph.\n\nSecond paragraph.",
        EntryStatus::complete, 1);
    response.created_at = 0;

    EXPECT_EQ(session_markdown("Plan", {&response, 1}),
        "<!-- CHA session: Plan -->\n"
        "\n`Guide` · First paragraph.  \nSecond paragraph.\n");
}

TEST(SessionMarkdown, OmitsTransientCoverMarkers) {
    TranscriptEntry response = make_character_entry(
        2, "guide", "Guide", "Off the record.", EntryStatus::complete, 1);
    response.created_at = 0;
    const std::vector<TranscriptEntry> entries{
        make_cover_marker(1),
        std::move(response),
        make_uncover_marker(3),
    };

    EXPECT_EQ(session_markdown("Plan", entries),
        "<!-- CHA session: Plan -->\n"
        "\n`Guide` · Off the record.\n");
}

} // namespace
} // namespace cha::web
