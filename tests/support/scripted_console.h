#pragma once

#include "ui/console/console_port.h"

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cha::test {

struct ScriptedWait {
    bool input{};
    bool notification{};
    // Keep this notification-only step armed for subsequent waits. This models
    // an event loop draining an asynchronous turn whose delta and terminal
    // events may arrive in either one wake or separate wakes.
    bool repeat{};
    bool signal{};
    bool closed{};
    bool failed{};
    // Models an asynchronous file read that was issued before backpressure
    // disabled input and completes during a later wait(false).
    bool bypass_input_suppression{};
    std::vector<std::string> lines;
};

class ScriptedConsole final : public ConsolePort {
public:
    explicit ScriptedConsole(std::vector<ScriptedWait> script = {}) {
        for (std::size_t index = 0; index < script.size(); ++index) {
            if (script[index].repeat && index + 1 != script.size()) {
                throw std::invalid_argument(
                    "A repeating scripted wait must be the final step");
            }
            script_.push_back(std::move(script[index]));
        }
    }

    void wake() noexcept override {
        {
            std::lock_guard lock(wake_mutex_);
            ++wake_count_;
        }
        wake_ready_.notify_all();
    }

    InputEvents wait(bool include_input = true) override {
        include_input_history.push_back(include_input);
        if (script_.empty()) {
            under_scripted = true;
            throw std::logic_error(
                "Scripted console wait script exhausted");
        }
        ScriptedWait step = script_.front();
        if (!step.repeat) {
            script_.pop_front();
        } else if (step.input || step.signal || step.failed
                   || !step.lines.empty() || !step.notification) {
            throw std::logic_error(
                "Only a notification wait without input, signal, or failure "
                "may repeat");
        }
        if (step.notification) {
            std::unique_lock lock(wake_mutex_);
            if (!wake_ready_.wait_for(
                    lock,
                    std::chrono::seconds(1),
                    [this] {
                        return wake_count_ > observed_wakes_;
                    })) {
                throw std::runtime_error(
                    "Timed out waiting for scripted notification");
            }
            observed_wakes_ = wake_count_;
        }
        if (!include_input && !step.bypass_input_suppression) {
            step.input = false;
            step.closed = false;
            step.lines.clear();
        }
        last_wait_included_input_ = include_input;
        pending_lines_ = std::move(step.lines);
        interrupt_ = step.signal;
        closed_ = closed_ || step.closed;
        return step.failed
            ? InputEvents::failure()
            : InputEvents::ready(
                step.input,
                step.closed,
                step.notification,
                step.signal);
    }

    std::vector<std::string> take_lines() override {
        if (!last_wait_included_input_) {
            ++suppressed_take_lines;
        }
        return std::exchange(pending_lines_, {});
    }

    bool input_closed() const override {
        return closed_;
    }

    bool take_interrupt() override {
        return std::exchange(interrupt_, false);
    }

    TranscriptSurface& transcript() override {
        return surface_;
    }

    TranscriptSurface& prompt() override {
        return prompt_surface_;
    }

    std::ostream& notices() override {
        return notices_;
    }

    bool flush() override {
        if (fail_flush_) {
            fail_flush_ = false;
            return false;
        }
        return true;
    }

    bool finish_transcript() override {
        if (fail_finish_) {
            fail_finish_ = false;
            return false;
        }
        return flush();
    }

    void fail_next_flush() {
        fail_flush_ = true;
    }

    void fail_next_finish() {
        fail_finish_ = true;
    }

    const std::string& transcript_output() const {
        return surface_.output;
    }

    std::string notice_output() const {
        return notices_.str();
    }

    bool under_scripted{};
    std::size_t suppressed_take_lines{};
    std::vector<bool> include_input_history;

private:
    class RecordingSurface final : public TranscriptSurface {
    public:
        void attributes(TranscriptAttributes) override {
        }
        void write(std::string_view text) override {
            output += text;
        }
        std::string output;
    };

    class PromptSurface final : public TranscriptSurface {
    public:
        explicit PromptSurface(std::ostream& output)
            : output_(output) {
        }
        void attributes(TranscriptAttributes) override {
        }
        void write(std::string_view text) override {
            output_ << text;
        }
    private:
        std::ostream& output_;
    };

    std::deque<ScriptedWait> script_;
    std::vector<std::string> pending_lines_;
    bool interrupt_{};
    bool closed_{};
    bool fail_flush_{};
    bool fail_finish_{};
    bool last_wait_included_input_{true};
    std::mutex wake_mutex_;
    std::condition_variable wake_ready_;
    std::size_t wake_count_{};
    std::size_t observed_wakes_{};
    RecordingSurface surface_;
    std::ostringstream notices_;
    PromptSurface prompt_surface_{notices_};
};

} // namespace cha::test
