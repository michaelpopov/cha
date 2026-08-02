#include "ui/console/transcript_emitter.h"
#include "support/test_transcript.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

class RecordingSurface final : public TranscriptSurface {
public:
    void attributes(TranscriptAttributes) override {
    }

    void write(std::string_view text) override {
        output += text;
    }

    std::string output;
};

TranscriptEntry streaming(EntryId id, std::string text) {
    return make_agent_entry(
        id,
        "guide",
        "Guide",
        std::move(text),
        EntryStatus::streaming);
}

TranscriptEntry complete(EntryId id, std::string text) {
    return make_agent_entry(
        id,
        "guide",
        "Guide",
        std::move(text),
        EntryStatus::complete);
}

struct TestTranscript {
    std::vector<TranscriptEntry> entries;
    std::size_t revision{};
    std::optional<EntryId> open_entry_id;
    std::size_t history_epoch{};

    [[nodiscard]] TranscriptView view() const noexcept {
        return {
            entries,
            revision,
            open_entry_id,
            history_epoch,
        };
    }
};

TEST(TranscriptEmitter, WritesOnlyStreamingSuffixAndClosesOnFinalization) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);

    const TestTranscript first{
        .entries = {streaming(1, "One")},
        .revision = 1,
        .open_entry_id = 1,
        .history_epoch = 1,
    };
    emitter.write(first.view());
    emitter.commit();
    EXPECT_EQ(surface.output, "[Guide] One");

    const TestTranscript second{
        .entries = {streaming(1, "One two")},
        .revision = 2,
        .open_entry_id = 1,
        .history_epoch = 1,
    };
    emitter.write(second.view());
    emitter.commit();
    EXPECT_EQ(surface.output, "[Guide] One two");

    const TestTranscript complete_view{
        .entries = {complete(1, "One two")},
        .revision = 3,
        .history_epoch = 1,
    };
    emitter.write(complete_view.view());
    emitter.commit();
    EXPECT_EQ(surface.output, "[Guide] One two\n\n");
}

TEST(TranscriptEmitter, WritesCompleteEntriesAndNeverRepeatsCommittedState) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    const TestTranscript transcript{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"guide", "Guide"}, "Question"),
            complete(2, "Answer"),
        },
        .revision = 2,
        .history_epoch = 1,
    };

    emitter.write(transcript.view());
    emitter.commit();
    emitter.write(transcript.view());
    emitter.commit();

    EXPECT_EQ(surface.output, "[You] Question\n\n[Guide] Answer\n\n");
}

TEST(TranscriptEmitter, MarksClearAndHandlesRestartedIds) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    const TestTranscript before{
        .entries = {complete(8, "Before")},
        .revision = 1,
        .history_epoch = 1,
    };
    emitter.write(before.view());
    emitter.commit();

    const TestTranscript after{
        .entries = {complete(1, "After")},
        .revision = 2,
        .history_epoch = 2,
    };
    emitter.write(after.view());
    emitter.commit();

    EXPECT_EQ(
        surface.output,
        "[Guide] Before\n\n--- cleared ---\n\n[Guide] After\n\n");
}

TEST(TranscriptEmitter, KeepsDiscardedPartialTextAndAppendsTheError) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    const TestTranscript partial{
        .entries = {streaming(2, "Partial")},
        .revision = 1,
        .open_entry_id = 2,
        .history_epoch = 1,
    };
    emitter.write(partial.view());
    emitter.commit();

    const TestTranscript failed{
        .entries = {make_error_entry(3, "Failed")},
        .revision = 2,
        .history_epoch = 1,
    };
    emitter.write(failed.view());
    emitter.commit();

    EXPECT_EQ(surface.output, "[Guide] Partial\n\n[Error] Failed\n\n");
}

TEST(TranscriptEmitter, PrintsRestoredHumansThenSkipsLiveOnesWhenEchoOff) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false, false);

    const TestTranscript restored{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"guide", "Guide"}, "Earlier"),
            complete(2, "Earlier answer"),
        },
        .revision = 2,
        .history_epoch = 1,
    };
    emitter.write(restored.view());
    emitter.commit();
    EXPECT_EQ(
        surface.output,
        "[You] Earlier\n\n[Guide] Earlier answer\n\n");

    const TestTranscript live{
        .entries = {
            test::human_entry(1, {"human", "You"}, {"guide", "Guide"}, "Earlier"),
            complete(2, "Earlier answer"),
            test::human_entry(3, {"human", "You"}, {"guide", "Guide"}, "Live prompt"),
            complete(4, "Live answer"),
        },
        .revision = 4,
        .history_epoch = 1,
    };
    emitter.write(live.view());
    emitter.commit();
    EXPECT_EQ(
        surface.output,
        "[You] Earlier\n\n[Guide] Earlier answer\n\n"
        "\n[Guide] Live answer\n\n");
}

TEST(TranscriptEmitter, DoesNotAdvanceWithoutCommit) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    const TestTranscript transcript{
        .entries = {complete(1, "Answer")},
        .revision = 1,
        .history_epoch = 1,
    };
    emitter.write(transcript.view());
    emitter.write(transcript.view());
    EXPECT_EQ(
        surface.output,
        "[Guide] Answer\n\n[Guide] Answer\n\n");
}

TEST(TranscriptEmitter, EmitsOffrecordMarkersOnceAndInOrder) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    const TestTranscript transcript{
        .entries = {
            make_hide_on_marker(1),
            make_hide_marker(2),
            make_hide_off_marker(3),
        },
        .revision = 3,
        .history_epoch = 1,
    };

    emitter.write(transcript.view());
    emitter.commit();
    emitter.write(transcript.view());
    emitter.commit();

    EXPECT_EQ(
        surface.output,
        "[hide-on]\n\n[hide]\n\n[hide-off]\n\n");
}

} // namespace
} // namespace cha
