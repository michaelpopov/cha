#include "ui/web/web_session_runtime.h"

#include "session/session_controller.h"
#include "ui/text/text_input.h"

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace cha::web {
namespace {

template<typename>
inline constexpr bool unsupported_web_command = false;

WebSettings validate_runtime_settings(WebSettings settings) {
    if (settings.command_batch_size == 0 || settings.event_batch_size == 0) {
        throw std::invalid_argument("Web runtime batch sizes must be positive");
    }
    return settings;
}

class SessionControllerAdapter final : public WebSessionController {
public:
    explicit SessionControllerAdapter(
        std::unique_ptr<SessionController> controller)
        : controller_(std::move(controller)) {}

    SessionUpdate handle_raw_input(std::string input) override {
        return cha::handle_text_input(*controller_, std::move(input));
    }
    SessionUpdate request_stop() override { return controller_->request_stop(); }
    SessionUpdate set_default_agent_id(std::string_view id) override {
        return controller_->set_default_agent_by_id(id);
    }
    SessionEventBatch receive(std::size_t max_events) override {
        return controller_->receive_events(max_events);
    }
    void shutdown() override { controller_->shutdown(); }

private:
    std::unique_ptr<SessionController> controller_;
};

static_assert(
    std::variant_size_v<WebCommand> == 3,
    "WebSessionRuntime must explicitly dispatch every WebCommand alternative");

} // namespace

WebSessionRuntime::WebSessionRuntime(
    WebControllerFactory factory,
    WebSettings settings)
    : settings_(validate_runtime_settings(std::move(settings))),
      commands_(settings_.command_queue_capacity),
      owner_(&WebSessionRuntime::owner_loop, this, std::move(factory)) {}

WebSessionRuntime::~WebSessionRuntime() {
    request_shutdown();
    if (owner_.joinable()) {
        owner_.join();
    }
}

CommandSubmitResult WebSessionRuntime::submit(
    WebCommand command,
    std::chrono::milliseconds deadline) {
    auto completion = std::make_shared<CommandCompletion>();
    bool wake_owner = false;
    {
        std::lock_guard lock(state_mutex_);
        if (stopping_) {
            return ErrorCode::session_not_live;
        }
        const CommandEnqueueResult enqueued =
            commands_.try_push({std::move(command), completion});
        if (!enqueued.accepted) {
            return ErrorCode::command_queue_full;
        }
        wake_owner = enqueued.wake_owner;
    }
    if (wake_owner) {
        notifier_.wake();
    }
    if (auto result = completion->wait_for(deadline)) {
        return std::move(*result);
    }
    return ErrorCode::command_timeout;
}

void WebSessionRuntime::request_shutdown() {
    {
        std::lock_guard lock(state_mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
    }
    notifier_.wake();
}

void WebSessionRuntime::owner_loop(WebControllerFactory factory) {
    std::unique_ptr<WebSessionController> controller = factory(notifier_);
    while (true) {
        std::size_t processed = 0;
        while (processed < settings_.command_batch_size) {
            {
                std::lock_guard lock(state_mutex_);
                if (stopping_) {
                    break;
                }
            }
            auto command = commands_.try_pop();
            if (!command) {
                break;
            }
            execute(*controller, std::move(*command));
            ++processed;
        }
        {
            std::lock_guard lock(state_mutex_);
            if (stopping_) {
                break;
            }
        }
        SessionEventBatch events =
            controller->receive(settings_.event_batch_size);
        apply_notice(events.update);
        if (events.update.end_session) {
            mark_stopping();
        }
        {
            std::lock_guard lock(state_mutex_);
            if (stopping_) {
                break;
            }
        }

        // A full batch may have left commands queued.  Continue immediately so
        // coalesced wakes cannot leave accepted commands idle until the next
        // deadline.
        if (processed == settings_.command_batch_size || events.full) {
            continue;
        }
        // There is no periodic owner-loop deadline. The connection-lifetime
        // block supplies its rearmable absolute deadline here; until then only
        // published work or shutdown should wake an idle runtime.
        (void)notifier_.wait_until(
            std::chrono::steady_clock::time_point::max());
    }
    while (auto command = commands_.try_pop()) {
        command->completion->complete(ErrorCode::session_not_live);
    }
    controller->shutdown();
}

void WebSessionRuntime::execute(
    WebSessionController& controller,
    OwnerCommand command) {
    SessionUpdate update = std::visit(
        [&controller](auto&& value) -> SessionUpdate {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, RawCommand>) {
                return controller.handle_raw_input(std::move(value.text));
            } else if constexpr (std::is_same_v<T, StopCommand>) {
                return controller.request_stop();
            } else if constexpr (std::is_same_v<T, SetDefaultAgentCommand>) {
                return controller.set_default_agent_id(value.persona_id);
            } else {
                static_assert(
                    unsupported_web_command<T>,
                    "Missing WebCommand dispatch in WebSessionRuntime");
            }
        },
        command.command);
    apply_notice(update);
    command.completion->complete(CommandResult{
        .clear_input = update.clear_input,
        .notice = update.notice,
    });
    if (update.end_session) {
        mark_stopping();
    }
}

void WebSessionRuntime::apply_notice(const SessionUpdate& update) {
    if (!update.notice) {
        return;
    }
    if (update.notice->empty()) {
        notice_.reset();
    } else {
        notice_ = *update.notice;
    }
}

void WebSessionRuntime::mark_stopping() {
    std::lock_guard lock(state_mutex_);
    stopping_ = true;
}

std::unique_ptr<WebSessionController> adapt_session_controller(
    std::unique_ptr<SessionController> controller) {
    return std::make_unique<SessionControllerAdapter>(std::move(controller));
}

} // namespace cha::web
