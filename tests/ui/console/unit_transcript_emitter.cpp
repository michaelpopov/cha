#include "ui/console/transcript_emitter.h"

#include <gtest/gtest.h>

#include <string>

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
        id, "guide", "Guide", std::move(text), EntryStatus::streaming);
}

TranscriptEntry complete(EntryId id, std::string text) {
    return make_agent_entry(
        id, "guide", "Guide", std::move(text), EntryStatus::complete);
}

TEST(TranscriptEmitter, WritesOnlyStreamingSuffixAndClosesOnFinalization) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);

    emitter.write({
        .entries = {streaming(1, "One")},
        .revision = 1,
        .open_entry_id = 1,
        .history_epoch = 1,
    });
    emitter.commit();
    EXPECT_EQ(surface.output, "[Guide] One");

    emitter.write({
        .entries = {streaming(1, "One two")},
        .revision = 2,
        .open_entry_id = 1,
        .history_epoch = 1,
    });
    emitter.commit();
    EXPECT_EQ(surface.output, "[Guide] One two");

    emitter.write({
        .entries = {complete(1, "One two")},
        .revision = 3,
        .history_epoch = 1,
    });
    emitter.commit();
    EXPECT_EQ(surface.output, "[Guide] One two\n\n");
}

TEST(TranscriptEmitter, WritesCompleteEntriesAndNeverRepeatsCommittedState) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    const TranscriptSnapshot snapshot{
        .entries = {
            make_human_entry(1, "guide", "Guide", "Question"),
            complete(2, "Answer"),
        },
        .revision = 2,
        .history_epoch = 1,
    };

    emitter.write(snapshot);
    emitter.commit();
    emitter.write(snapshot);
    emitter.commit();

    EXPECT_EQ(surface.output, "[You] Question\n\n[Guide] Answer\n\n");
}

TEST(TranscriptEmitter, MarksClearAndHandlesRestartedIds) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    emitter.write({
        .entries = {complete(8, "Before")},
        .revision = 1,
        .history_epoch = 1,
    });
    emitter.commit();
    emitter.write({
        .entries = {complete(1, "After")},
        .revision = 2,
        .history_epoch = 2,
    });
    emitter.commit();

    EXPECT_EQ(
        surface.output,
        "[Guide] Before\n\n--- cleared ---\n\n[Guide] After\n\n");
}

TEST(TranscriptEmitter, KeepsDiscardedPartialTextAndAppendsTheError) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    emitter.write({
        .entries = {streaming(2, "Partial")},
        .revision = 1,
        .open_entry_id = 2,
        .history_epoch = 1,
    });
    emitter.commit();
    emitter.write({
        .entries = {make_error_entry(3, "Failed")},
        .revision = 2,
        .history_epoch = 1,
    });
    emitter.commit();

    EXPECT_EQ(surface.output, "[Guide] Partial\n\n[Error] Failed\n\n");
}

TEST(TranscriptEmitter, PrintsRestoredHumansThenSkipsLiveOnesWhenEchoOff) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false, false);

    emitter.write({
        .entries = {
            make_human_entry(1, "guide", "Guide", "Earlier"),
            complete(2, "Earlier answer"),
        },
        .revision = 2,
        .history_epoch = 1,
    });
    emitter.commit();
    EXPECT_EQ(
        surface.output,
        "[You] Earlier\n\n[Guide] Earlier answer\n\n");

    emitter.write({
        .entries = {
            make_human_entry(1, "guide", "Guide", "Earlier"),
            complete(2, "Earlier answer"),
            make_human_entry(3, "guide", "Guide", "Live prompt"),
            complete(4, "Live answer"),
        },
        .revision = 4,
        .history_epoch = 1,
    });
    emitter.commit();
    EXPECT_EQ(
        surface.output,
        "[You] Earlier\n\n[Guide] Earlier answer\n\n"
        "\n[Guide] Live answer\n\n");
}

TEST(TranscriptEmitter, DoesNotAdvanceWithoutCommit) {
    RecordingSurface surface;
    TranscriptEmitter emitter(surface, false);
    const TranscriptSnapshot snapshot{
        .entries = {complete(1, "Answer")},
        .revision = 1,
        .history_epoch = 1,
    };
    emitter.write(snapshot);
    emitter.write(snapshot);
    EXPECT_EQ(
        surface.output,
        "[Guide] Answer\n\n[Guide] Answer\n\n");
}

} // namespace
} // namespace cha
