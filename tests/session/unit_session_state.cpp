#include "session/session_state.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

constexpr RequestId active_request = 7;
constexpr EntryId open_entry = 42;
constexpr std::size_t epoch = 3;
const ParticipantId default_agent = "ada";

TranscriptEntry streaming_answer(std::string text) {
    return {
        .id = open_entry,
        .kind = EntryKind::agent,
        .participant_id = default_agent,
        .display_name = "Ada",
        .text = std::move(text),
        .status = EntryStatus::streaming,
        .request_id = active_request,
    };
}

TranscriptView view_of(
    const std::vector<TranscriptEntry>& entries,
    std::size_t revision) {
    return {entries, revision, open_entry, epoch};
}

AppendGenerationView answering(std::string_view reasoning = {}) {
    return {
        .active = true,
        .request_id = active_request,
        .phase = ResponsePhase::answering,
        .reasoning_text = reasoning,
    };
}

SessionStateCursor answering_cursor(
    std::size_t revision,
    std::size_t answer_length,
    std::size_t reasoning_length = 0) {
    return {
        .revision = revision,
        .history_epoch = epoch,
        .entry_count = 1,
        .open_entry_id = open_entry,
        .default_agent_id = default_agent,
        .request_id = active_request,
        .phase = ResponsePhase::answering,
        .reasoning_length = reasoning_length,
        .answer_length = answer_length,
    };
}

} // namespace

TEST(SessionTextAppend, ProvesAnswerGrowthAndAdvancesOnlyTheTextCursor) {
    const std::vector<TranscriptEntry> entries{streaming_answer("Hello there")};
    const SessionStateCursor cursor = answering_cursor(4, 5);

    const auto projection = session_text_append_since(
        view_of(entries, 5), answering(), default_agent, cursor);

    ASSERT_TRUE(projection);
    EXPECT_EQ(
        projection->append,
        (SessionTextAppend{EntryTextTarget{open_entry}, " there"}));
    EXPECT_EQ(projection->cursor.revision, 5U);
    EXPECT_EQ(projection->cursor.answer_length, entries.front().text.size());
    // Everything the proof compared must survive unchanged, or the next append
    // would be measured against a cursor that silently moved.
    EXPECT_EQ(projection->cursor.history_epoch, cursor.history_epoch);
    EXPECT_EQ(projection->cursor.entry_count, cursor.entry_count);
    EXPECT_EQ(projection->cursor.reasoning_length, cursor.reasoning_length);
}

TEST(SessionTextAppend, ProvesReasoningGrowthBeforeAnyAnswerEntryExists) {
    const std::vector<TranscriptEntry> entries;
    const TranscriptView transcript{entries, 9, std::nullopt, epoch};
    const SessionStateCursor cursor{
        .revision = 9,
        .history_epoch = epoch,
        .entry_count = 0,
        .open_entry_id = std::nullopt,
        .default_agent_id = default_agent,
        .request_id = active_request,
        .phase = ResponsePhase::reasoning,
        .reasoning_length = 4,
    };
    const AppendGenerationView generation{
        .active = true,
        .request_id = active_request,
        .phase = ResponsePhase::reasoning,
        .reasoning_text = "step one",
    };

    const auto projection = session_text_append_since(
        transcript, generation, default_agent, cursor);

    ASSERT_TRUE(projection);
    EXPECT_EQ(
        projection->append,
        (SessionTextAppend{ReasoningTextTarget{active_request}, " one"}));
    EXPECT_EQ(projection->cursor.reasoning_length, 8U);
}

TEST(SessionTextAppend, RefusesWhenAnythingOtherThanTheTextCouldHaveChanged) {
    const std::vector<TranscriptEntry> entries{streaming_answer("Hello there")};
    const TranscriptView grown = view_of(entries, 5);

    // A cleared transcript reuses entry IDs, so a stale epoch can never be
    // treated as an append.
    SessionStateCursor stale_epoch = answering_cursor(4, 5);
    stale_epoch.history_epoch = epoch - 1;
    EXPECT_FALSE(session_text_append_since(
        grown, answering(), default_agent, stale_epoch));

    // The default agent is presentation state carried in the same snapshot.
    EXPECT_FALSE(session_text_append_since(
        grown, answering(), "grace", answering_cursor(4, 5)));

    SessionStateCursor other_count = answering_cursor(4, 5);
    other_count.entry_count = 2;
    EXPECT_FALSE(session_text_append_since(
        grown, answering(), default_agent, other_count));

    SessionStateCursor other_phase = answering_cursor(4, 5);
    other_phase.phase = ResponsePhase::reasoning;
    EXPECT_FALSE(session_text_append_since(
        grown, answering(), default_agent, other_phase));

    SessionStateCursor other_request = answering_cursor(4, 5);
    other_request.request_id = active_request + 1;
    EXPECT_FALSE(session_text_append_since(
        grown, answering(), default_agent, other_request));

    // Reasoning may arrive after answer chunks; it must not hide inside an
    // answer append.
    EXPECT_FALSE(session_text_append_since(
        grown, answering("late thought"), default_agent, answering_cursor(4, 5)));

    // Text that did not grow, and a revision that did not move, are both
    // ambiguous rather than an empty append.
    EXPECT_FALSE(session_text_append_since(
        grown, answering(), default_agent,
        answering_cursor(4, entries.front().text.size())));
    EXPECT_FALSE(session_text_append_since(
        view_of(entries, 4), answering(), default_agent, answering_cursor(4, 5)));

    // An inactive generation has no appendable target at all.
    AppendGenerationView inactive = answering();
    inactive.active = false;
    EXPECT_FALSE(session_text_append_since(
        grown, inactive, default_agent, answering_cursor(4, 5)));
}

TEST(SessionTextAppend, ConsumesTheCursorItsProducerBuilt) {
    SessionState state{
        .default_agent_id = default_agent,
        .transcript = {streaming_answer("Hello")},
        .revision = 4,
        .open_entry_id = open_entry,
        .history_epoch = epoch,
        .generation = {
            .active = true,
            .request_id = active_request,
            .agent_id = default_agent,
            .agent_name = "Ada",
            .phase = ResponsePhase::answering,
        },
    };
    const auto cursor = session_state_cursor(state);
    ASSERT_TRUE(cursor);

    state.transcript.front().text = "Hello there";
    ++state.revision;
    const auto projection = session_text_append_since(
        view_of(state.transcript, state.revision),
        answering(),
        state.default_agent_id,
        *cursor);

    ASSERT_TRUE(projection);
    EXPECT_EQ(
        projection->append,
        (SessionTextAppend{EntryTextTarget{open_entry}, " there"}));
    // The advanced cursor must be the one the producer would build from the
    // state that append describes.
    EXPECT_EQ(projection->cursor, session_state_cursor(state));
}

} // namespace cha
