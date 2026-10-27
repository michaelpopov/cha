#pragma once

#include "characters/character_config.h"
#include "providers/maintenance.h"
#include "providers/model_backend.h"
#include "providers/sse_framer.h"

#include <nlohmann/json.hpp>

namespace cha {

std::string build_google_request_body(
    const GenerationRequest& input, const ModelBackendConfig& config,
    std::string_view system_prompt, RequestTextSizes* text_sizes = nullptr);

// One response's semantic state. Both SSE and complete JSON use the same parser.
// Keep parts intact: thought signatures must stay on their original parts.
class GoogleStreamDecoder final : public StreamingResponseDecoder {
public:
    explicit GoogleStreamDecoder(const GenerationDeltaSink& on_delta,
        bool collect_tool_calls = false,
        std::size_t argument_limit = ordinary_tool_argument_limit,
        bool maintenance = false);
    GoogleStreamDecoder(GenerationDeltaSink&&, bool = false,
        std::size_t = ordinary_tool_argument_limit, bool = false) = delete;

    GoogleStreamDecoder(const GoogleStreamDecoder&) = delete;
    GoogleStreamDecoder& operator=(const GoogleStreamDecoder&) = delete;

    void consume(std::string_view bytes) override;
    StreamDecodeResult finish() override;

private:
    friend GenerationResult decode_google_response(std::string_view,
        const GenerationDeltaSink&, bool, std::size_t, bool);
    bool process_json(std::string_view data);

    const GenerationDeltaSink* on_delta_;
    bool collect_tool_calls_;
    std::size_t argument_limit_;
    bool maintenance_;
    SseFramer framer_;
    std::string protocol_error_;
    std::string finish_reason_;
    bool received_answer_{};
    bool refused_{};
    bool prompt_blocked_{};
    GenerationTokenUsage usage_;
    std::optional<std::uint64_t> candidate_tokens_;
    std::optional<std::uint64_t> thought_tokens_;
    nlohmann::json content_{{"role", "model"}, {"parts", nlohmann::json::array()}};
};

GenerationResult decode_google_response(std::string_view body,
    const GenerationDeltaSink& on_delta, bool collect_tool_calls = false,
    std::size_t argument_limit = ordinary_tool_argument_limit,
    bool maintenance = false);

} // namespace cha
