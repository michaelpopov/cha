#include "agents/completion_backend.h"
#include "session/session_controller.h"
#include "session/session_database.h"
#include "support/scripted_console.h"
#include "support/test_backends.h"
#include "ui/console/console_session.h"
#include "ui/console/transcript_emitter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cha {
namespace {

class TemporaryJournal {
public:
    TemporaryJournal()
        : path(std::filesystem::temp_directory_path()
            / ("cha_console_session_"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())
                + ".sqlite3")) {
        if (!create_session_database(
                path,
                {
                    .id = "console-test",
                    .room = "test",
                    .label = "Console test",
                })) {
            throw std::runtime_error("Failed to create temporary journal");
        }
    }

    ~TemporaryJournal() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

class EchoBackend final : public CompletionBackend {
public:
    explicit EchoBackend(bool wait_for_cancel = false)
        : wait_for_cancel_(wait_for_cancel) {
    }

    RequestPayload prepare(
        const CompletionRequest& request,
        const TranscriptReadView&) override {
        return {.bytes = request.prompt.text};
    }

    CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        if (wait_for_cancel_) {
            while (!cancellation.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return {CompletionOutcome::cancelled, {}};
        }
        on_delta({
            CompletionDeltaKind::answer,
            "Answer to " + payload.bytes,
        });
        return {};
    }

    AgentRuntimeInfo info() const override {
        return {
            .persona = {.id = "guide", .name = "Guide"},
            .model = "test",
            .api = "test://console",
            .streaming = true,
        };
    }

private:
    bool wait_for_cancel_{};
};

std::unique_ptr<SessionController> controller(
    TemporaryJournal& journal,
    bool wait_for_cancel = false,
    SessionRestore restored = {}) {
    return SessionController::from_backends_for_testing(
        test::one_backend(
            std::make_unique<EchoBackend>(wait_for_cancel)),
        journal.path,
        std::move(restored));
}

TEST(ConsoleSession, DrainsSeveralPipedPromptsInOrderAfterEof) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"one", "two"},
        },
        {.notification = true},
        {.notification = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_EQ(
        port.transcript_output(),
        "[You] one\n\n[Guide] Answer to one\n\n"
        "[You] two\n\n[Guide] Answer to two\n\n");
    EXPECT_EQ(
        port.notice_output().find(generation_in_progress_notice),
        std::string::npos);
    EXPECT_FALSE(port.under_scripted);
}

TEST(ConsoleSession, EmitsRestoredHistoryBeforeWaiting) {
    TemporaryJournal journal;
    SessionRestore restored{
        .entries = {
            make_human_entry(1, "guide", "Guide", "Earlier"),
            make_agent_entry(
                2,
                "guide",
                "Guide",
                "Earlier answer",
                EntryStatus::complete),
        },
        .next_entry_id = 3,
    };
    auto session_controller =
        controller(journal, false, std::move(restored));
    test::ScriptedConsole port({
        {.signal = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_EQ(
        port.transcript_output(),
        "[You] Earlier\n\n[Guide] Earlier answer\n\n");
    ASSERT_EQ(port.include_input_history.size(), 1U);
}

TEST(ConsoleSession, EofWaitsForTheActiveTurnToComplete) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"question"},
        },
        {.notification = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(
        port.transcript_output().find("Answer to question"),
        std::string::npos);
}

TEST(ConsoleSession, StopWithAnArgumentWaitsInTheQueue) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"question", "/stop now"},
        },
        {.notification = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(
        port.transcript_output().find("Answer to question"),
        std::string::npos);
    EXPECT_NE(
        port.notice_output().find("Command does not accept arguments"),
        std::string::npos);
    EXPECT_EQ(port.notice_output().find("Stopping"), std::string::npos);
}

TEST(ConsoleSession, ExitDropsTheRemainingQueue) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    test::ScriptedConsole port({
        {
            .input = true,
            .lines = {"/exit", "never"},
        },
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_TRUE(port.transcript_output().empty());
}

TEST(ConsoleSession, StopActsImmediatelyDuringGeneration) {
    TemporaryJournal journal;
    auto session_controller = controller(journal, true);
    test::ScriptedConsole port({
        {.input = true, .lines = {"question"}},
        {.input = true, .closed = true, .lines = {"/stop "}},
        {.notification = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(port.notice_output().find("Stopping"), std::string::npos);
    EXPECT_NE(
        port.notice_output().find("Generation stopped"),
        std::string::npos);
}

TEST(ConsoleSession, InterruptWhileGeneratingCancelsWithoutExiting) {
    TemporaryJournal journal;
    auto session_controller = controller(journal, true);
    test::ScriptedConsole port({
        {.input = true, .lines = {"question"}},
        {.signal = true},
        {.notification = true},
        {.closed = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(port.notice_output().find("Stopping"), std::string::npos);
    EXPECT_NE(
        port.notice_output().find("Generation stopped"),
        std::string::npos);
}

TEST(ConsoleSession, InterruptWhileIdleExitsImmediately) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    test::ScriptedConsole port({
        {.signal = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
}

TEST(ConsoleSession, HandlesSignalAndNotificationFromTheSameWait) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    test::ScriptedConsole port({
        {.input = true, .lines = {"question"}},
        {.notification = true, .signal = true},
        {.closed = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(
        port.transcript_output().find("Answer to question"),
        std::string::npos);
}

TEST(ConsoleSession, ClosedControllerChannelEndsTheSession) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    session_controller->shutdown();
    test::ScriptedConsole port({
        {.notification = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(port, *session_controller, emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_FALSE(port.under_scripted);
}

TEST(ConsoleSession, PipeBackpressureSuppressesInputOnlyWhileQueueIsFull) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"one", "two", "three"},
        },
        {.notification = true},
        {.notification = true},
        {.notification = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter,
        {
            .backpressure_stdin = true,
            .queue_limit = 1,
        });

    EXPECT_EQ(session.run(), 0);
    ASSERT_GE(port.include_input_history.size(), 2U);
    EXPECT_TRUE(port.include_input_history.front());
    EXPECT_NE(
        std::find(
            port.include_input_history.begin(),
            port.include_input_history.end(),
            false),
        port.include_input_history.end());
}

TEST(ConsoleSession, InteractiveInputRemainsEnabledWhenQueueIsFull) {
    TemporaryJournal journal;
    auto session_controller = controller(journal);
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"one", "two"},
        },
        {.notification = true},
        {.notification = true},
    });
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter,
        {
            .backpressure_stdin = false,
            .queue_limit = 1,
        });

    EXPECT_EQ(session.run(), 0);
    EXPECT_TRUE(std::all_of(
        port.include_input_history.begin(),
        port.include_input_history.end(),
        [](bool included) { return included; }));
}

TEST(ConsoleSession, ReportsWaitAndFlushFailures) {
    TemporaryJournal wait_journal;
    auto wait_controller = controller(wait_journal);
    test::ScriptedConsole wait_port({{.failed = true}});
    TranscriptEmitter wait_emitter(wait_port.transcript(), false);
    ConsoleSession wait_session(
        wait_port, *wait_controller, wait_emitter);
    EXPECT_EQ(wait_session.run(), 1);
    EXPECT_NE(wait_port.notice_output().find("wait failed"), std::string::npos);

    TemporaryJournal flush_journal;
    SessionRestore restored{
        .entries = {
            make_human_entry(1, "guide", "Guide", "Undelivered"),
        },
        .next_entry_id = 2,
    };
    auto flush_controller =
        controller(flush_journal, false, std::move(restored));
    test::ScriptedConsole flush_port;
    flush_port.fail_next_flush();
    TranscriptEmitter flush_emitter(flush_port.transcript(), false);
    ConsoleSession flush_session(
        flush_port, *flush_controller, flush_emitter);
    EXPECT_EQ(flush_session.run(), 1);
    EXPECT_NE(
        flush_port.notice_output().find("Failed to write"),
        std::string::npos);
    const std::string first_write = flush_port.transcript_output();
    flush_emitter.write(flush_controller->transcript().snapshot());
    EXPECT_EQ(
        flush_port.transcript_output(),
        first_write + first_write);
}

} // namespace
} // namespace cha
