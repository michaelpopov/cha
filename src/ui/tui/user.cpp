#include "ui/tui/user.h"

#include "session/session_controller.h"
#include "ui/tui/terminal.h"
#include "ui/tui/tui.h"
#include "ui/tui/user_session.h"
#include "util/uv_event_loop.h"

#include <uv.h>

#include <csignal>
#include <exception>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>

namespace cha {
namespace {

std::runtime_error uv_error(const char* action, int status) {
    return std::runtime_error(
        std::string(action) + ": " + uv_strerror(status));
}

void require_uv(int status, const char* action) {
    if (status < 0) {
        throw uv_error(action, status);
    }
}

struct TuiEvents {
    bool input{};
    bool input_closed{};
    bool notification{};
    bool resize{};
    bool failed{};
};

class TuiEventSource {
public:
    explicit TuiEventSource(UvEventLoop& event_loop)
        : event_loop_(event_loop) {
        try {
            require_uv(
                uv_poll_init(
                    &event_loop_.native_loop(),
                    &input_,
                    STDIN_FILENO),
                "Failed to initialize terminal input wait");
            input_initialized_ = true;
            input_.data = this;
            require_uv(
                uv_poll_start(
                    &input_,
                    UV_READABLE | UV_DISCONNECT,
                    receive_input),
                "Failed to wait for terminal input");
            input_started_ = true;

            require_uv(
                uv_signal_init(
                    &event_loop_.native_loop(),
                    &resize_),
                "Failed to initialize terminal resize signal");
            resize_initialized_ = true;
            resize_.data = this;
            require_uv(
                uv_signal_start(
                    &resize_,
                    receive_resize,
                    SIGWINCH),
                "Failed to wait for terminal resize");
            resize_started_ = true;
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~TuiEventSource() {
        cleanup();
    }

    TuiEvents wait() {
        while (true) {
            const bool notification =
                event_loop_.take_notification();
            if (input_ready_ || input_closed_ || notification
                || resize_ready_ || failed_) {
                return {
                    .input = std::exchange(input_ready_, false),
                    .input_closed =
                        std::exchange(input_closed_, false),
                    .notification = notification,
                    .resize =
                        std::exchange(resize_ready_, false),
                    .failed = failed_,
                };
            }
            event_loop_.run_once();
        }
    }

private:
    static void receive_input(
        uv_poll_t* handle,
        int status,
        int events) {
        auto& source =
            *static_cast<TuiEventSource*>(handle->data);
        if (status < 0) {
            source.failed_ = true;
            return;
        }
        source.input_ready_ =
            source.input_ready_
            || (events & UV_READABLE) != 0;
        source.input_closed_ =
            source.input_closed_
            || (events & UV_DISCONNECT) != 0;
    }

    static void receive_resize(
        uv_signal_t* handle,
        int number) {
        if (number == SIGWINCH) {
            static_cast<TuiEventSource*>(handle->data)
                ->resize_ready_ = true;
        }
    }

    void cleanup() noexcept {
        if (resize_initialized_) {
            if (resize_started_) {
                (void)uv_signal_stop(&resize_);
                resize_started_ = false;
            }
            event_loop_.close(
                *reinterpret_cast<uv_handle_t*>(&resize_));
            resize_initialized_ = false;
        }
        if (input_initialized_) {
            if (input_started_) {
                (void)uv_poll_stop(&input_);
                input_started_ = false;
            }
            event_loop_.close(
                *reinterpret_cast<uv_handle_t*>(&input_));
            input_initialized_ = false;
        }
    }

    UvEventLoop& event_loop_;
    uv_poll_t input_{};
    uv_signal_t resize_{};
    bool input_initialized_{};
    bool input_started_{};
    bool resize_initialized_{};
    bool resize_started_{};
    bool input_ready_{};
    bool input_closed_{};
    bool resize_ready_{};
    bool failed_{};
};

} // namespace

// Coordinate semantic events here while leaving libuv handles and mutable UI
// state to their respective modules.
void run_user(
    Terminal& terminal,
    SessionController& controller,
    UvEventLoop& event_loop,
    User user) {

    std::exception_ptr failure;
    {
        Tui tui(terminal);
        UserSession session(tui, controller, std::move(user));
        try {
            TuiEventSource events(event_loop);
            session.render();

            while (session.running()) {
                const TuiEvents ready = events.wait();
                if (ready.resize) {
                    session.resize();
                }
                if (ready.failed) {
                    session.report_terminal_failure();
                    session.render_if_needed();
                    break;
                }

                if (ready.input_closed) {
                    session.close_terminal();
                }

                if (ready.notification) {
                    session.receive_responses();
                }

                if (session.running() && ready.input) {
                    session.receive_terminal_input();
                }

                session.render_if_needed();
            }

            session.shutdown();
        } catch (...) {
            failure = std::current_exception();
        }
    }

    if (failure) {
        terminal.restore();
        try {
            controller.shutdown();
        } catch (...) {
            // Preserve the operation failure that ended the session.
        }
        std::rethrow_exception(failure);
    }
}

} // namespace cha
