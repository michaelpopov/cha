#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "transcript/transcript.h"
#include "ui/render/transcript_writer.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

TranscriptEntry human(EntryId id, std::string text) {
    return make_human_entry(id, "guide-id", "Guide", std::move(text));
}

class RecordingSurface final : public TranscriptSurface {
public:
    void attributes(TranscriptAttributes value) override {
        current = value;
        operations.push_back({value, {}});
    }

    void write(std::string_view text) override {
        operations.push_back({current, std::string(text)});
        output += text;
    }

    struct Operation {
        TranscriptAttributes attributes;
        std::string text;
    };

    TranscriptAttributes current{TranscriptAttributes::normal};
    std::vector<Operation> operations;
    std::string output;
};

TEST(ShowAddressing, DependsOnForumPersonasAndForeignHistory) {
    const ForumPersonas single({
        PersonaInfo{
            .id = "guide-id",
            .name = "Guide",
        }
    });
    const ForumPersonas multi({
        PersonaInfo{
            .id = "guide-id",
            .name = "Guide",
        },
        PersonaInfo{
            .id = "ismael-id",
            .name = "Ismael",
        },
    });

    Transcript empty;
    EXPECT_FALSE(show_addressing(single, empty.view()));
    EXPECT_TRUE(show_addressing(multi, empty.view()));

    Transcript foreign;
    foreign.replace_entries({
        make_human_entry(1, "guide-id", "Guide", "Question", 1),
        make_agent_entry(
            2, "former-id", "Former", "Answer", EntryStatus::complete, 1),
    });
    EXPECT_TRUE(show_addressing(single, foreign.view()));

    foreign.clear();
    EXPECT_FALSE(show_addressing(single, foreign.view()));
}

TEST(TranscriptLabel, FormatsEveryEntryKind) {
    EXPECT_EQ(
        transcript_entry_label(human(1, "Question"), false),
        "[You] ");
    EXPECT_EQ(
        transcript_entry_label(
            make_agent_entry(
                2,
                "agent-id",
                "Guide",
                "Answer",
                EntryStatus::complete),
            false),
        "[Guide] ");
    EXPECT_EQ(
        transcript_entry_label(make_notice_entry(3, "Notice"), false),
        "[System] ");
    EXPECT_EQ(
        transcript_entry_label(make_hide_on_marker(4), false),
        "[hide-on]");
    EXPECT_EQ(
        transcript_entry_label(make_error_entry(4, "Failure"), false),
        "[Error] ");
}

TEST(TranscriptRendering, RendersMulticastPromptsAsOrdinaryAddressedPrompts) {
    RecordingSurface surface;
    write_transcript_entry(
        surface,
        make_human_entry(1, "one-id", "One", "Question", 1),
        true);
    write_transcript_entry(
        surface,
        make_human_entry(2, "two-id", "Two", "Question", 2),
        true);

    EXPECT_EQ(
        surface.output,
        "[You → One] Question[You → Two] Question");
}

TEST(TranscriptRendering, RendersAnEmptyMarkerWithoutATrailingSeparator) {
    RecordingSurface surface;
    write_transcript_entry(surface, make_hide_off_marker(1), false);

    EXPECT_EQ(surface.output, "[hide-off]");
}

TEST(TranscriptRendering, LabelsEphemeralReasoningAndRestoresNormalAttributes) {
    RecordingSurface surface;
    write_active_response(
        surface,
        "Guide",
        "Compare constraints",
        "Use the second option");

    EXPECT_EQ(
        surface.output,
        "[Guide]\n[Reasoning]\nCompare constraints\n\n"
        "Use the second option");
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);

    bool reasoning_was_dim = false;
    bool answer_was_normal = false;
    for (const RecordingSurface::Operation& operation : surface.operations) {
        if (operation.text == "Compare constraints") {
            reasoning_was_dim =
                operation.attributes == TranscriptAttributes::dim;
        }
        if (operation.text == "Use the second option") {
            answer_was_normal =
                operation.attributes == TranscriptAttributes::normal;
        }
    }
    EXPECT_TRUE(reasoning_was_dim);
    EXPECT_TRUE(answer_was_normal);

    write_transcript_entry(surface, human(2, "Ordinary"), false);
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);
    EXPECT_EQ(
        surface.operations.back().attributes,
        TranscriptAttributes::normal);
}

TEST(TranscriptRendering, AnswerSuffixAndInputInitializationRestoreNormalAttributes) {
    RecordingSurface surface;
    surface.current = TranscriptAttributes::bold_dim;
    write_transcript_suffix(surface, " more answer");
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);
    ASSERT_GE(surface.operations.size(), 3U);
    EXPECT_EQ(
        surface.operations[surface.operations.size() - 2].attributes,
        TranscriptAttributes::normal);

    surface.current = TranscriptAttributes::dim;
    initialize_transcript_surface(surface);
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);
}

} // namespace
} // namespace cha
