#pragma once

#include "agents/character_config.h"
#include "agents/model_backend.h"
#include "agents/model_context.h"
#include "agents/provider_response.h"

#include <string>
#include <string_view>

namespace cha {

// Builds a Responses API request body for one generation. Uses the same
// projected history as Chat Completions, with the system prompt as top-level
// instructions and store always false.
std::string build_responses_request_body(
    const GenerationRequest& input,
    const ModelBackendConfig& config,
    std::string_view system_prompt);

// Decodes one Responses API SSE stream into answer/refusal text. Search queries,
// search lifecycle, annotations, and reasoning events are ignored.
class ResponsesStreamDecoder final : public StreamingResponseDecoder {
public:
    explicit ResponsesStreamDecoder(const GenerationDeltaSink& on_delta);
    ResponsesStreamDecoder(GenerationDeltaSink&&) = delete;

    ResponsesStreamDecoder(const ResponsesStreamDecoder&) = delete;
    ResponsesStreamDecoder& operator=(const ResponsesStreamDecoder&) = delete;

    void consume(std::string_view bytes) override;
    StreamDecodeResult finish() override;

private:
    bool handle_event_json(std::string_view data);
    void emit_answer(std::string text);

    const GenerationDeltaSink* on_delta_;
    SseFramer framer_;
    std::string protocol_error_;
    bool done_{};
    bool completed_successfully_{};
    bool received_answer_{};
    bool describe_response_{true};
    GenerationTokenUsage usage_;
};

// Decodes one complete non-streaming Responses body into answer/refusal text.
GenerationResult decode_responses_response(
    std::string_view body,
    const GenerationDeltaSink& on_delta);

} // namespace cha
