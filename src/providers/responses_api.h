#pragma once

#include "characters/character_config.h"
#include "characters/model_context.h"
#include "providers/model_backend.h"
#include "providers/sse_framer.h"

#include <nlohmann/json.hpp>
#include <map>
#include <string>
#include <string_view>

namespace cha {

// Builds a Responses API request body for one generation. Uses the same
// projected history as Chat Completions, with the system prompt as top-level
// instructions and store always false.
std::string build_responses_request_body(
    const GenerationRequest& input,
    const ModelBackendConfig& config,
    std::string_view system_prompt,
    RequestTextSizes* text_sizes = nullptr);

// Decodes answer/refusal text and retains completed output items for tool
// continuation. Provider-hosted search lifecycle events are ignored.
class ResponsesStreamDecoder final : public StreamingResponseDecoder {
public:
    explicit ResponsesStreamDecoder(const GenerationDeltaSink& on_delta,
        bool collect_tool_calls = false);
    ResponsesStreamDecoder(GenerationDeltaSink&&, bool = false) = delete;

    ResponsesStreamDecoder(const ResponsesStreamDecoder&) = delete;
    ResponsesStreamDecoder& operator=(const ResponsesStreamDecoder&) = delete;

    void consume(std::string_view bytes) override;
    StreamDecodeResult finish() override;

private:
    bool handle_event_json(std::string_view data);
    void emit_answer(std::string text);

    const GenerationDeltaSink* on_delta_;
    bool collect_tool_calls_;
    SseFramer framer_;
    std::string protocol_error_;
    bool done_{};
    bool completed_successfully_{};
    bool received_answer_{};
    bool describe_response_{true};
    GenerationTokenUsage usage_;
    std::map<int, nlohmann::json> output_items_;
    nlohmann::json output_ = nlohmann::json::array();
};

// Decodes one complete non-streaming Responses body into answer/refusal text.
GenerationResult decode_responses_response(
    std::string_view body,
    const GenerationDeltaSink& on_delta,
    bool collect_tool_calls = false);

} // namespace cha
