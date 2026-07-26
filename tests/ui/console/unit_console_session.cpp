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
    explicit EchoBackend(
        bool wait_for_cancel = false,
        std::string id = "guide",
        std::string name = "Guide")
        : wait_for_cancel_(wait_for_cancel),
          id_(std::move(id)),
          name_(std::move(name)) {
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
            .persona = {.id = id_, .name = name_},
            .model = "test",
            .api = "test://console",
            .streaming = true,
        };
    }

private:
    bool wait_for_cancel_{};
    std::string id_;
    std::string name_;
};

class TestController {
public:
    TestController(
        TemporaryJournal& journal,
        WakeNotifier& notifier,
        bool wait_for_cancel,
        SessionRestore restored)
        : controller_(SessionController::from_backends_for_testing(
            test::one_backend(
                std::make_unique<EchoBackend>(wait_for_cancel)),
            journal.path,
            notifier,
            std::move(restored))) {
    }

    TestController(
        TemporaryJournal& journal,
        WakeNotifier& notifier,
        std::vector<std::unique_ptr<CompletionBackend>> backends)
        : controller_(SessionController::from_backends_for_testing(
            std::move(backends),
            journal.path,
            notifier)) {
    }

    SessionController& operator*() const {
        return *controller_;
    }

    SessionController* operator->() const {
        return controller_.get();
    }

private:
    std::unique_ptr<SessionController> controller_;
};

TestController controller(
    TemporaryJournal& journal,
    WakeNotifier& notifier,
    bool wait_for_cancel = false,
    SessionRestore restored = {}) {
    return TestController(
        journal,
        notifier,
        wait_for_cancel,
        std::move(restored));
}

TEST(ConsoleSession, DrainsSeveralPipedPromptsInOrderAfterEof) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"one", "two"},
        },
        {.notification = true, .repeat = true},
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

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

TEST(ConsoleSession, ShowsPromptWhenIdleNotWhileGenerating) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {
            .input = true,
            .lines = {"question"},
        },
        {.notification = true},
        {.signal = true},
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter,
        {.show_prompt = true});

    EXPECT_EQ(session.run(), 0);
    EXPECT_EQ(port.notice_output(), "@Guide> @Guide> ");
    EXPECT_EQ(
        port.transcript_output(),
        "[You] question\n\n[Guide] Answer to question\n\n")
        << "default emitter still records human prompts for pipes/tests";
}

TEST(ConsoleSession, PromptTracksTheCurrentDefaultAgent) {
    TemporaryJournal journal;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<EchoBackend>());
    backends.push_back(std::make_unique<EchoBackend>(
        false, "ismael", "Ismael"));
    test::ScriptedConsole port({
        {
            .input = true,
            .lines = {"/@Ismael"},
        },
        {.signal = true},
    });
    TestController session_controller(
        journal,
        port,
        std::move(backends));
    TranscriptEmitter emitter(port.transcript(), true);
    ConsoleSession session(
        port,
        *session_controller,
        emitter,
        {.show_prompt = true});

    EXPECT_EQ(session.run(), 0);
    EXPECT_EQ(
        port.notice_output(),
        "@Guide> Default agent is now Ismael\n@Ismael> ");
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
    test::ScriptedConsole port({
        {.signal = true},
    });
    auto session_controller =
        controller(journal, port, false, std::move(restored));
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_EQ(
        port.transcript_output(),
        "[You] Earlier\n\n[Guide] Earlier answer\n\n");
    ASSERT_EQ(port.include_input_history.size(), 1U);
}

TEST(ConsoleSession, EofWaitsForTheActiveTurnToComplete) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"question"},
        },
        {.notification = true, .repeat = true},
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(
        port.transcript_output().find("Answer to question"),
        std::string::npos);
}

TEST(ConsoleSession, StopWithAnArgumentWaitsInTheQueue) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"question", "/stop now"},
        },
        {.notification = true, .repeat = true},
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

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
    test::ScriptedConsole port({
        {
            .input = true,
            .lines = {"/exit", "never"},
        },
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_TRUE(port.transcript_output().empty());
}

TEST(ConsoleSession, ExitWithAnArgumentUsesSharedCommandRules) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {
            .input = true,
            .closed = true,
            .lines = {"/exit now", "still runs"},
        },
        {.notification = true, .repeat = true},
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(
        port.notice_output().find("Command does not accept arguments"),
        std::string::npos);
    EXPECT_NE(
        port.transcript_output().find("Answer to still runs"),
        std::string::npos);
}

TEST(ConsoleSession, StopActsImmediatelyDuringGeneration) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {.input = true, .lines = {"question"}},
        {.input = true, .closed = true, .lines = {"/stop "}},
        {.notification = true},
    });
    auto session_controller = controller(journal, port, true);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(port.notice_output().find("Stopping"), std::string::npos);
    EXPECT_NE(
        port.notice_output().find("Generation stopped"),
        std::string::npos);
}

TEST(ConsoleSession, InterruptWhileGeneratingCancelsWithoutExiting) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {.input = true, .lines = {"question"}},
        {.signal = true},
        {.notification = true},
        {.closed = true},
    });
    auto session_controller = controller(journal, port, true);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(port.notice_output().find("Stopping"), std::string::npos);
    EXPECT_NE(
        port.notice_output().find("Generation stopped"),
        std::string::npos);
}

TEST(ConsoleSession, InterruptWhileIdleExitsImmediately) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {.signal = true},
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
}

TEST(ConsoleSession, HandlesSignalAndNotificationFromTheSameWait) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {.input = true, .lines = {"question"}},
        {.notification = true, .signal = true},
        {.closed = true},
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(
        port.transcript_output().find("Answer to question"),
        std::string::npos);
}

TEST(ConsoleSession, ClosedControllerChannelEndsTheSession) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {.notification = true},
    });
    auto session_controller = controller(journal, port);
    session_controller->shutdown();
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    EXPECT_FALSE(port.under_scripted);
}

TEST(ConsoleSession, PipeBackpressureSuppressesInputOnlyWhileQueueIsFull) {
    TemporaryJournal journal;
    // Standard input stays open until the last step so a suppressed read can
    // only be attributed to the full queue, never to end of input.
    test::ScriptedConsole port({
        {.input = true, .lines = {"one", "two", "three"}},
        {.notification = true, .repeat = true, .closed = true},
    });
    auto session_controller = controller(journal, port);
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

TEST(ConsoleSession, DoesNotConsumeAFileReadCompletedDuringBackpressure) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {.input = true, .lines = {"one", "two"}},
        {
            .input = true,
            .notification = true,
            .closed = true,
            .bypass_input_suppression = true,
            .lines = {"deferred"},
        },
        {
            .input = true,
            .closed = true,
            .lines = {"deferred"},
        },
        {.notification = true, .repeat = true},
    });
    auto session_controller = controller(journal, port);
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
    EXPECT_EQ(port.suppressed_take_lines, 0U);
    EXPECT_EQ(
        port.transcript_output(),
        "[You] one\n\n[Guide] Answer to one\n\n"
        "[You] two\n\n[Guide] Answer to two\n\n"
        "[You] deferred\n\n[Guide] Answer to deferred\n\n");
}

TEST(ConsoleSession, InteractiveInputRemainsEnabledWhenQueueIsFull) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {.input = true, .lines = {"one", "two"}},
        {.notification = true, .repeat = true, .closed = true},
    });
    auto session_controller = controller(journal, port);
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
    ASSERT_GE(port.include_input_history.size(), 2U);
    EXPECT_TRUE(port.include_input_history[0]);
    EXPECT_TRUE(port.include_input_history[1])
        << "interactive input remains armed while the prompt queue is full";
}

// A closed standard input watcher must remain disabled; otherwise every wait
// can return immediately and spin the loop for the rest of the turn.
TEST(ConsoleSession, StopsReadingStandardInputOnceItIsExhausted) {
    TemporaryJournal journal;
    test::ScriptedConsole port({
        {.input = true, .closed = true, .lines = {"one"}},
        {.notification = true, .repeat = true},
    });
    auto session_controller = controller(journal, port);
    TranscriptEmitter emitter(port.transcript(), false);
    ConsoleSession session(
        port,
        *session_controller,
        emitter);

    EXPECT_EQ(session.run(), 0);
    ASSERT_GE(port.include_input_history.size(), 2U);
    EXPECT_TRUE(port.include_input_history.front());
    EXPECT_TRUE(std::none_of(
        port.include_input_history.begin() + 1,
        port.include_input_history.end(),
        [](bool included) { return included; }));
}

TEST(ConsoleSession, ReportsWaitAndFlushFailures) {
    TemporaryJournal wait_journal;
    test::ScriptedConsole wait_port({{.failed = true}});
    auto wait_controller = controller(wait_journal, wait_port);
    TranscriptEmitter wait_emitter(wait_port.transcript(), false);
    ConsoleSession wait_session(
        wait_port,
        *wait_controller,
        wait_emitter);
    EXPECT_EQ(wait_session.run(), 1);
    EXPECT_NE(wait_port.notice_output().find("wait failed"), std::string::npos);

    TemporaryJournal flush_journal;
    SessionRestore restored{
        .entries = {
            make_human_entry(1, "guide", "Guide", "Undelivered"),
        },
        .next_entry_id = 2,
    };
    test::ScriptedConsole flush_port;
    auto flush_controller =
        controller(
            flush_journal,
            flush_port,
            false,
            std::move(restored));
    flush_port.fail_next_flush();
    TranscriptEmitter flush_emitter(flush_port.transcript(), false);
    ConsoleSession flush_session(
        flush_port,
        *flush_controller,
        flush_emitter);
    EXPECT_EQ(flush_session.run(), 1);
    EXPECT_NE(
        flush_port.notice_output().find("Failed to write"),
        std::string::npos);
    const std::string first_write = flush_port.transcript_output();
    flush_emitter.write(flush_controller->transcript().snapshot());
    EXPECT_EQ(
        flush_port.transcript_output(),
        first_write + first_write);

    TemporaryJournal finish_journal;
    test::ScriptedConsole finish_port({{.closed = true}});
    auto finish_controller =
        controller(finish_journal, finish_port);
    finish_port.fail_next_finish();
    TranscriptEmitter finish_emitter(finish_port.transcript(), false);
    ConsoleSession finish_session(
        finish_port,
        *finish_controller,
        finish_emitter);
    EXPECT_EQ(finish_session.run(), 1);
    EXPECT_NE(
        finish_port.notice_output().find("Failed to write"),
        std::string::npos);
}

} // namespace
} // namespace cha
