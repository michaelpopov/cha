#include "web/live_session.h"

#include "web/sse_mailbox.h"

#include <memory>
#include <utility>
#include <variant>

namespace cha::web {

CommandSubmitResult LiveSession::connect_sse(
    std::chrono::milliseconds deadline) {
    CommandSubmitResult submitted = submit(SseConnectCommand{}, deadline);
    SseConnectResult* owner = std::get_if<SseConnectResult>(&submitted);
    if (!owner) return submitted;
    auto mailbox = std::static_pointer_cast<SseMailbox>(sse_adapter_);
    if (!mailbox) {
        mailbox = std::make_shared<SseMailbox>(output_);
        sse_adapter_ = mailbox;
    }
    owner->mailbox = mailbox;
    owner->stream = mailbox->listen();
    return std::move(*owner);
}

} // namespace cha::web
