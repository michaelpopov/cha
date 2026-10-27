#pragma once

#include "characters/character_config.h"
#include "providers/maintenance.h"
#include "providers/model_backend.h"
#include "providers/sse_framer.h"

#include <nlohmann/json.hpp>

namespace cha {

inline constexpr std::string_view anthropic_api_version = "2026-10-21";

std::string build_anthropic_request_body(
    const GenerationRequest& input, const ModelBackendConfig& config,
    std::string_view system_prompt, RequestTextSizes* text_sizes = nullptr);

// One HTTP response. The original content blocks also form the tool continuation.
class AnthropicStreamDecoder final : public StreamingResponseDecoder {
public:
    explicit AnthropicStreamDecoder(const GenerationDeltaSink& on_delta,
        bool collect_tool_calls = false,
        std::size_t argument_limit = ordinary_tool_argument_limit,
        bool maintenance = false);
    AnthropicStreamDecoder(GenerationDeltaSink&&, bool = false,
        std::size_t = ordinary_tool_argument_limit, bool = false) = delete;

    AnthropicStreamDecoder(const AnthropicStreamDecoder&) = delete;
    AnthropicStreamDecoder& operator=(const AnthropicStreamDecoder&) = delete;

    void consume(std::string_view bytes) override;
    StreamDecodeResult finish() override;

private:
    friend GenerationResult decode_anthropic_response(std::string_view,
        const GenerationDeltaSink&, bool, std::size_t, bool);
    bool process_json(std::string_view data);
    void update_usage(const nlohmann::json& usage);
    void emit_block(const nlohmann::json& block);
    GenerationResult completed_result();

    const GenerationDeltaSink* on_delta_;
    bool collect_tool_calls_;
    std::size_t argument_limit_;
    bool maintenance_;
    SseFramer framer_;
    nlohmann::json message_{{"role", "assistant"}, {"content", nlohmann::json::array()}};
    GenerationTokenUsage usage_;
    std::optional<std::uint64_t> uncached_input_;
    std::string protocol_error_;
    std::string stop_reason_;
    std::optional<std::size_t> active_block_;
    std::string partial_input_;
    bool invalid_input_{};
    bool oversized_{};
    bool received_answer_{};
    bool started_{};
    bool stopped_{};
};

GenerationResult decode_anthropic_response(std::string_view body,
    const GenerationDeltaSink& on_delta, bool collect_tool_calls = false,
    std::size_t argument_limit = ordinary_tool_argument_limit,
    bool maintenance = false);

} // namespace cha
