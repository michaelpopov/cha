#include "providers/anthropic_api.h"

#include "providers/tool_calls.h"
#include "util/json_serialization.h"

#include <stdexcept>

namespace cha {
namespace {
using Json = nlohmann::json;
constexpr int default_max_tokens = 64000;

void update_count(const Json& usage, const char* field, std::optional<std::uint64_t>& count) {
    if (const auto value = usage.find(field); value != usage.end()) {
        if (value->is_null()) return;
        if (!value->is_number_integer() || *value < 0) {
            throw std::invalid_argument("Invalid Anthropic usage");
        }
        count = value->get<std::uint64_t>();
    }
}

void validate_block(const Json& block) {
    const auto type = block.at("type").get<std::string>();
    if (type == "text") block.at("text").get_ref<const std::string&>();
    else if (type == "thinking") {
        block.at("thinking").get_ref<const std::string&>();
        block.at("signature").get_ref<const std::string&>();
    } else if (type == "redacted_thinking") block.at("data").get_ref<const std::string&>();
    else if (type == "tool_use") {
        block.at("id").get_ref<const std::string&>();
        block.at("name").get_ref<const std::string&>();
        if (!block.at("input").is_object()) throw std::invalid_argument("Invalid Anthropic tool input");
    } else throw std::invalid_argument("Unsupported Anthropic content block: " + type);
}
} // namespace

std::string build_anthropic_request_body(
    const GenerationRequest& input, const ModelBackendConfig& config,
    std::string_view system_prompt, RequestTextSizes* text_sizes) {
    if (text_sizes) *text_sizes = {};
    Json body{{"model", config.model}, {"stream", config.stream},
        {"max_tokens", config.max_tokens.value_or(default_max_tokens)}, {"messages", Json::array()}};
    for (const auto& message : project_model_context(input, system_prompt)) {
        const bool system = message.role == ModelRole::system;
        if (text_sizes) {
            (system ? text_sizes->system_prompt_bytes : text_sizes->conversation_bytes) += message.content.size();
        }
        if (message.content.empty()) continue;
        if (system) body["system"] = message.content;
        else {
            const auto role = message.role == ModelRole::assistant ? "assistant" : "user";
            auto& messages = body["messages"];
            if (messages.empty() || messages.back().at("role") != role) {
                messages.push_back({{"role", role}, {"content", Json::array()}});
            }
            messages.back()["content"].push_back({{"type", "text"}, {"text", message.content}});
        }
    }
    if (input.maintenance_tool) add_maintenance_tools(body, ProviderApi::anthropic);
    if (input.web_search_tool) add_web_search_tool(body, ProviderApi::anthropic);
    if (input.web_read_tool) add_web_read_tool(body, ProviderApi::anthropic);
    if (!input.maintenance_tool && input.summary_tool) add_summary_tools(body, ProviderApi::anthropic);
    if (input.include_tool_instructions) update_tool_instructions(body, ProviderApi::anthropic, text_sizes, input.summary_context);
    return dump_json(body, "Model request");
}

AnthropicStreamDecoder::AnthropicStreamDecoder(const GenerationDeltaSink& on_delta,
    bool collect_tool_calls, std::size_t argument_limit, bool maintenance)
    : on_delta_(&on_delta), collect_tool_calls_(collect_tool_calls),
      argument_limit_(argument_limit), maintenance_(maintenance) {}

void AnthropicStreamDecoder::update_usage(const Json& usage) {
    if (!usage.is_object()) throw std::invalid_argument("Invalid Anthropic usage");
    update_count(usage, "input_tokens", uncached_input_);
    update_count(usage, "output_tokens", usage_.output_tokens);
    update_count(usage, "cache_read_input_tokens", usage_.cache_read_tokens);
    update_count(usage, "cache_creation_input_tokens", usage_.cache_write_tokens);
    if (uncached_input_) usage_.input_tokens = *uncached_input_
        + usage_.cache_read_tokens.value_or(0) + usage_.cache_write_tokens.value_or(0);
}

void AnthropicStreamDecoder::emit_block(const Json& block) {
    const auto type = block.at("type").get<std::string>();
    if (type != "text" && type != "thinking") return;
    const auto& text = block.at(type == "text" ? "text" : "thinking").get_ref<const std::string&>();
    if (text.empty()) return;
    if (type == "text") received_answer_ = true;
    (*on_delta_)({type == "text" ? GenerationDeltaKind::answer : GenerationDeltaKind::reasoning, text});
}

void AnthropicStreamDecoder::consume(std::string_view bytes) {
    framer_.consume(bytes, [this](std::string_view data) { return process_json(data); });
}

bool AnthropicStreamDecoder::process_json(std::string_view data) {
    if (!protocol_error_.empty()) return true;
    try {
        const auto value = Json::parse(data);
        const auto type = value.at("type").get<std::string>();
        if (type == "error") {
            protocol_error_ = value.at("error").value("message", std::string{}).substr(0, 512);
            if (protocol_error_.empty()) protocol_error_ = "Anthropic returned an error";
        } else if (type == "message_start") {
            if (started_ || stopped_) throw std::invalid_argument("Duplicate Anthropic message_start");
            const auto& message = value.at("message");
            if (message.at("type") != "message" || message.at("role") != "assistant"
                || !message.at("content").is_array() || !message.at("content").empty()) {
                throw std::invalid_argument("Invalid Anthropic message_start");
            }
            started_ = true;
            if (message.contains("usage")) update_usage(message.at("usage"));
        } else if (type == "content_block_start" || type == "content_block_delta" || type == "content_block_stop") {
            if (!started_ || stopped_ || !stop_reason_.empty()) throw std::invalid_argument("Unexpected Anthropic content event");
            const auto& index_value = value.at("index");
            if (!index_value.is_number_integer() || index_value < 0) throw std::invalid_argument("Invalid Anthropic block index");
            const auto index = index_value.get<std::size_t>();
            auto& content = message_["content"];
            if (type == "content_block_start") {
                if (active_block_ || index != content.size()) throw std::invalid_argument("Invalid Anthropic block index");
                const auto& block = value.at("content_block");
                validate_block(block);
                content.push_back(block);
                active_block_ = index;
                partial_input_.clear();
                emit_block(block);
            } else {
                if (!active_block_ || *active_block_ != index) throw std::invalid_argument("Invalid Anthropic block index");
                auto& block = content.at(index);
                if (type == "content_block_stop") {
                    if (!partial_input_.empty() && !oversized_) {
                        auto input = Json::parse(partial_input_, nullptr, false);
                        if (!input.is_object()) invalid_input_ = true;
                        else block["input"] = std::move(input);
                    }
                    active_block_.reset();
                    partial_input_.clear();
                } else {
                    const auto& delta = value.at("delta");
                    const auto delta_type = delta.at("type").get<std::string>();
                    const auto block_type = block.at("type").get<std::string>();
                    if (delta_type == "input_json_delta" && block_type == "tool_use") {
                        const auto& fragment = delta.at("partial_json").get_ref<const std::string&>();
                        if (fragment.size() > argument_limit_ - partial_input_.size()) {
                            oversized_ = true;
                            partial_input_.clear();
                        } else if (!oversized_) partial_input_ += fragment;
                    } else if ((delta_type == "text_delta" && block_type == "text")
                        || (delta_type == "thinking_delta" && block_type == "thinking")
                        || (delta_type == "signature_delta" && block_type == "thinking")) {
                        const char* field = delta_type == "text_delta" ? "text"
                            : delta_type == "thinking_delta" ? "thinking" : "signature";
                        const auto& fragment = delta.at(field).get_ref<const std::string&>();
                        block[field].get_ref<std::string&>() += fragment;
                        if (delta_type != "signature_delta") emit_block(Json{{"type", block_type}, {field, fragment}});
                    } else throw std::invalid_argument("Invalid Anthropic content delta: " + delta_type);
                }
            }
        } else if (type == "message_delta") {
            if (!started_ || stopped_) throw std::invalid_argument("Unexpected Anthropic message_delta");
            const auto& delta = value.at("delta");
            if (!delta.is_object()) throw std::invalid_argument("Invalid Anthropic message_delta");
            if (delta.contains("stop_reason") && !delta.at("stop_reason").is_null()) {
                stop_reason_ = delta.at("stop_reason").get<std::string>();
            }
            if (value.contains("usage")) update_usage(value.at("usage"));
        } else if (type == "message_stop") {
            if (!started_ || stopped_) throw std::invalid_argument("Unexpected Anthropic message_stop");
            stopped_ = true;
        }
        // Ping and unknown event types do not terminate the response.
    } catch (const Json::exception&) {
        protocol_error_ = "Malformed Anthropic response JSON";
    } catch (const std::invalid_argument& error) {
        protocol_error_ = error.what();
    }
    return true;
}

GenerationResult AnthropicStreamDecoder::completed_result() {
    std::string reason;
    bool refused = false;
    if (stop_reason_ == "end_turn" || stop_reason_ == "stop_sequence") reason = "stop";
    else if (stop_reason_ == "tool_use") reason = "tool_use";
    else if (stop_reason_ == "max_tokens" || stop_reason_ == "model_context_window_exceeded") reason = "length";
    else if (stop_reason_ == "refusal") { reason = "content_filter"; refused = true; }
    else if (protocol_error_.empty() && !stop_reason_.empty()) protocol_error_ = "Unsupported Anthropic stop_reason: " + stop_reason_;

    bool has_tool = false;
    for (const auto& block : message_.at("content")) has_tool |= block.is_object() && block.contains("type") && block.at("type") == "tool_use";
    GenerationResult result;
    const auto fail = [&](std::string message) { return GenerationResult{GenerationOutcome::protocol_error, std::move(message), usage_}; };
    if (!protocol_error_.empty()) result = fail(protocol_error_);
    else if (has_tool && reason == "length") result = fail(maintenance_
        ? std::string(maintenance_output_limit_message) : "Tool response ended incomplete");
    else if (!stopped_ || reason.empty()) result = fail("Anthropic response ended before a terminal status");
    else if (has_tool && reason != "tool_use") result = fail("Tool response ended incomplete");
    else if (oversized_) result = fail("Tool arguments too large");
    else if (active_block_ || invalid_input_) result = fail("Anthropic response contained incomplete or invalid content blocks");
    else {
        result = tool_call_result(message_, ProviderApi::anthropic, received_answer_, usage_, true,
            "Response completed without answer content", reason, argument_limit_, maintenance_);
        if (result.outcome == GenerationOutcome::completed && reason == "tool_use" && result.tool_calls.empty()) {
            result = fail("Anthropic tool_use response contained no calls");
        } else if (!collect_tool_calls_ && !result.tool_calls.empty()) {
            result = fail("Model requested a tool when tools were unavailable");
        }
    }
    result.finish_reason = reason;
    result.refused = refused;
    return result;
}

StreamDecodeResult AnthropicStreamDecoder::finish() {
    framer_.finish([this](std::string_view data) { return process_json(data); });
    return {completed_result(), false};
}

GenerationResult decode_anthropic_response(std::string_view body,
    const GenerationDeltaSink& on_delta, bool collect_tool_calls,
    std::size_t argument_limit, bool maintenance) {
    AnthropicStreamDecoder decoder(on_delta, collect_tool_calls, argument_limit, maintenance);
    try {
        const auto message = Json::parse(body);
        if (message.contains("error")) {
            decoder.protocol_error_ = message.at("error").value("message", std::string{}).substr(0, 512);
            if (decoder.protocol_error_.empty()) decoder.protocol_error_ = "Anthropic returned an error";
        } else {
            if (message.at("type") != "message" || message.at("role") != "assistant"
                || !message.at("content").is_array()) throw std::invalid_argument("Invalid Anthropic message");
            decoder.stop_reason_ = message.at("stop_reason").get<std::string>();
            decoder.started_ = decoder.stopped_ = true;
            if (message.contains("usage")) decoder.update_usage(message.at("usage"));
            decoder.message_["content"] = message.at("content");
            for (const auto& block : decoder.message_.at("content")) {
                // Defer invalid tool input until the terminal length status is handled.
                if (block.value("type", "") == "tool_use") {
                    if (!block.contains("input") || !block.at("input").is_object()) decoder.invalid_input_ = true;
                } else validate_block(block);
                decoder.emit_block(block);
            }
        }
    } catch (const Json::exception&) {
        decoder.protocol_error_ = "Malformed Anthropic response JSON";
    } catch (const std::invalid_argument& error) {
        decoder.protocol_error_ = error.what();
    }
    return decoder.completed_result();
}

} // namespace cha
