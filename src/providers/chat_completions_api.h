#pragma once

#include "agents/character_config.h"
#include "providers/model_backend.h"
#include "providers/sse_framer.h"

#include <string>
#include <string_view>

namespace cha {

// Builds a Chat Completions request body from one projected generation.
std::string build_chat_completions_request_body(
    const GenerationRequest& input,
    const ModelBackendConfig& config,
    std::string_view system_prompt);

// Decodes one Chat Completions response delivered as server-sent events,
// reporting reasoning and answer text through the delta sink as it arrives.
//
// One decoder serves one response: consume() accepts the response bytes split at
// any positions, and finish() classifies the completed stream. The decoder knows
// nothing about HTTP, curl, or cancellation; its caller owns the transfer and
// decides whether a decoded outcome is the final one.
class ChatCompletionsStreamDecoder final : public StreamingResponseDecoder {
public:
    // The decoder borrows the sink for the whole response, so callers pass one
    // that outlives it. Binding a temporary is rejected rather than left to
    // dangle through the transfer.
    ChatCompletionsStreamDecoder(
        ReasoningFormat format,
        const GenerationDeltaSink& on_delta);
    ChatCompletionsStreamDecoder(ReasoningFormat, GenerationDeltaSink&&) = delete;

    ChatCompletionsStreamDecoder(const ChatCompletionsStreamDecoder&) = delete;
    ChatCompletionsStreamDecoder& operator=(
        const ChatCompletionsStreamDecoder&) = delete;

    // Decodes every complete event the received bytes now contain. Bytes that
    // arrive after the provider's end marker are ignored.
    void consume(std::string_view bytes) override;

    // Decodes a trailing event the stream never terminated, then reports whether
    // the response was a complete answer.
    StreamDecodeResult finish() override;

private:
    bool handle_event_data(std::string_view data);
    void emit(GenerationDeltaKind kind, std::string text);

    bool received_output() const noexcept {
        return received_reasoning_ || received_answer_;
    }

    ReasoningFormat format_;
    const GenerationDeltaSink* on_delta_;
    SseFramer framer_;
    std::string protocol_error_;
    bool done_{};
    bool received_reasoning_{};
    bool received_answer_{};
    GenerationTokenUsage usage_;
};

// Decodes one complete non-streaming response body, reporting its reasoning and
// answer text through the delta sink. Both response forms carry the same message
// fields, so both apply the same reasoning-format rules.
GenerationResult decode_chat_completions_response(
    std::string_view body,
    ReasoningFormat format,
    const GenerationDeltaSink& on_delta);

} // namespace cha
