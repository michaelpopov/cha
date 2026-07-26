#pragma once

#include "ui/console/console_port.h"

#include <deque>
#include <ostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cha::test {

struct ScriptedWait {
    bool input{};
    bool notification{};
    bool signal{};
    bool closed{};
    bool failed{};
    std::vector<std::string> lines;
};

class ScriptedConsole final : public ConsolePort {
public:
    explicit ScriptedConsole(std::vector<ScriptedWait> script = {}) {
        for (ScriptedWait& step : script) {
            script_.push_back(std::move(step));
        }
    }

    InputEvents wait(
        int notification_fd,
        bool include_input = true) override {
        include_input_history.push_back(include_input);
        if (script_.empty()) {
            under_scripted = true;
            closed_ = true;
            return InputEvents::ready(false, true);
        }
        ScriptedWait step = std::move(script_.front());
        script_.pop_front();
        if (step.notification) {
            pollfd descriptor{notification_fd, POLLIN, 0};
            if (::poll(&descriptor, 1, 1000) != 1) {
                throw std::runtime_error(
                    "Timed out waiting for scripted notification");
            }
        }
        if (!include_input) {
            step.input = false;
            step.closed = false;
            step.lines.clear();
        }
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

    void fail_next_flush() {
        fail_flush_ = true;
    }

    const std::string& transcript_output() const {
        return surface_.output;
    }

    std::string notice_output() const {
        return notices_.str();
    }

    bool under_scripted{};
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

    std::deque<ScriptedWait> script_;
    std::vector<std::string> pending_lines_;
    bool interrupt_{};
    bool closed_{};
    bool fail_flush_{};
    RecordingSurface surface_;
    std::ostringstream notices_;
};

} // namespace cha::test
