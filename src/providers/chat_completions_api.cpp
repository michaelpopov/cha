#include "providers/chat_completions_api.h"

#include "util/json_serialization.h"
#include "providers/tool_calls.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {

namespace {

using Json = nlohmann::json;

std::optional<std::uint64_t> token_count(
    const Json& usage,
    std::string_view field) {
    const auto value = usage.find(field);
    if (value == usage.end() || !value->is_number_unsigned()) {
        return std::nullopt;
    }
    return value->get<std::uint64_t>();
}

GenerationTokenUsage chat_token_usage(const Json& response) {
    const auto usage = response.find("usage");
    if (usage == response.end() || !usage->is_object()) {
        return {};
    }
    std::optional<std::uint64_t> cache_read_tokens;
    std::optional<std::uint64_t> cache_write_tokens;
    const auto details = usage->find("prompt_tokens_details");
    if (details != usage->end() && details->is_object()) {
        cache_read_tokens = token_count(*details, "cached_tokens");
        cache_write_tokens = token_count(*details, "cache_write_tokens");
    }
    if (!cache_read_tokens) {
        cache_read_tokens = token_count(*usage, "prompt_cache_hit_tokens");
    }
    return {
        .input_tokens = token_count(*usage, "prompt_tokens"),
        .output_tokens = token_count(*usage, "completion_tokens"),
        .cache_read_tokens = cache_read_tokens,
        .cache_write_tokens = cache_write_tokens,
    };
}

std::string_view role_name(ModelRole role) {
    switch (role) {
    case ModelRole::system: return "system";
    case ModelRole::user: return "user";
    case ModelRole::assistant: return "assistant";
    }
    throw std::logic_error("Unknown model context role");
}

// Interprets one provider message or delta object: the reasoning field named by
// the configured format, then answer content. Returns the protocol error the
// object carried, or an empty string. 'emit' receives non-empty text only.
template<typename Emit>
std::string process_response_object(
    const Json& object,
    ReasoningFormat format,
    const Emit& emit) {
    std::string protocol_error;
    const auto emit_field = [&object, &emit, &protocol_error](
                                std::string_view field,
                                bool strict) -> bool {
        const auto iterator = object.find(field);
        if (iterator == object.end() || iterator->is_null()) {
            return false;
        }
        if (!iterator->is_string()) {
            if (strict && protocol_error.empty()) {
                protocol_error =
                    "Reasoning field '" + std::string(field)
                    + "' was not a string or null";
            }
            return false;
        }
        std::string text = iterator->get<std::string>();
        if (text.empty()) {
            return false;
        }
        emit(GenerationDeltaKind::reasoning, std::move(text));
        return true;
    };

    switch (format) {
    case ReasoningFormat::automatic:
        if (!emit_field("reasoning_content", false)) {
            if (!emit_field("reasoning", false)) {
                (void)emit_field("reasoning_text", false);
            }
        }
        break;
    case ReasoningFormat::none:
        break;
    case ReasoningFormat::reasoning_content:
        (void)emit_field("reasoning_content", true);
        break;
    case ReasoningFormat::reasoning:
        (void)emit_field("reasoning", true);
        break;
    }

    const auto content = object.find("content");
    if (content != object.end() && content->is_string()) {
        std::string text = content->get<std::string>();
        if (!text.empty()) {
            emit(GenerationDeltaKind::answer, std::move(text));
        }
    }

    return protocol_error;
}

} // namespace

std::string build_chat_completions_request_body(
    const GenerationRequest& input,
    const ModelBackendConfig& config,
    std::string_view system_prompt,
    RequestTextSizes* text_sizes) {
    if (text_sizes) *text_sizes = {};
    Json messages = Json::array();
    for (const ModelMessage& message :
         project_model_context(input, system_prompt)) {
        if (text_sizes) {
            auto& size = message.role == ModelRole::system
                ? text_sizes->system_prompt_bytes
                : text_sizes->conversation_bytes;
            size += message.content.size();
        }
        messages.push_back({
            {"role", role_name(message.role)},
            {"content", message.content},
        });
    }

    Json body{
        {"model", config.model},
        {"stream", config.stream},
        {"messages", std::move(messages)},
    };
    if (is_direct_openai_host(config.host) || is_openrouter_host(config.host)) {
        // Both services accept priority as the Fast mode tier name.
        body["service_tier"] = "priority";
    }
    if (config.temperature) {
        body["temperature"] = *config.temperature;
    }
    if (config.max_tokens) {
        body["max_tokens"] = *config.max_tokens;
    }
    if (config.stream) {
        body["stream_options"] = Json{{"include_usage", true}};
    }
    if (!config.reasoning_effort.empty()) {
        body["reasoning_effort"] = config.reasoning_effort;
    }
    if (!config.openrouter_targets.empty()) {
        if (!valid_openrouter_targets(config)) {
            throw std::logic_error("Invalid OpenRouter inference targets");
        }
        body["provider"] = {
            {"order", config.openrouter_targets},
            {"allow_fallbacks", false},
        };
    }
    if (config.web_search != WebSearchMode::off) {
        if (!is_openrouter_host(config.host)) {
            throw std::logic_error(
                "Chat Completions web search requires OpenRouter");
        }
        body["tools"] = Json::array({
            Json{{"type", "openrouter:web_search"}},
        });
        body["tool_choice"] = config.web_search == WebSearchMode::required
            ? "required"
            : "auto";
    }
    if (!input.run.prompt_cache_key.empty()
        && config.cache_retention != CacheRetention::off) {
        if (is_direct_openai_host(config.host)) {
            body["prompt_cache_key"] = input.run.prompt_cache_key;
        } else if (is_openrouter_host(config.host)) {
            body["session_id"] = input.run.prompt_cache_key;
        }
    }

    if (input.web_search_tool) add_web_search_tool(body, config.api);
    return dump_json(body, "Model request");
}

ChatCompletionsStreamDecoder::ChatCompletionsStreamDecoder(
    ReasoningFormat format,
    const GenerationDeltaSink& on_delta,
    bool collect_tool_calls)
    : format_(format),
      on_delta_(&on_delta),
      collect_tool_calls_(collect_tool_calls) {
}

void ChatCompletionsStreamDecoder::consume(std::string_view bytes) {
    if (done_) {
        return;
    }
    framer_.consume(bytes, [this](std::string_view data) {
        return handle_event_data(data);
    });
}

StreamDecodeResult ChatCompletionsStreamDecoder::finish() {
    if (!done_) {
        framer_.finish([this](std::string_view data) {
            return handle_event_data(data);
        });
    }

    if (!protocol_error_.empty()) {
        return {{GenerationOutcome::protocol_error, protocol_error_, usage_}, true};
    }
    if (!done_) {
        return {{
            GenerationOutcome::protocol_error,
            received_output()
                ? "Streaming response ended before [DONE]"
                : "Streaming response was not valid SSE",
            usage_,
        }, true};
    }
    return {tool_call_result(message_, ProviderApi::chat_completions,
        received_answer_, usage_, collect_tool_calls_,
        "Streaming response completed without answer content", finish_reason_), false};
}

void ChatCompletionsStreamDecoder::accumulate_delta(const Json& delta) {
    for (const auto field : {"content", "reasoning_content", "reasoning", "reasoning_text"}) {
        if (delta.contains(field) && delta[field].is_string()) {
            if (!message_.contains(field)) message_[field] = "";
            message_[field].get_ref<std::string&>() += delta[field].get<std::string>();
        }
    }
    if (delta.contains("reasoning_details") && delta["reasoning_details"].is_array()) {
        auto& details = message_["reasoning_details"];
        if (details.is_null()) details = Json::array();
        for (const auto& part : delta["reasoning_details"]) {
            const auto index = part.value("index", 0);
            if (index < 0 || index >= 32) throw std::invalid_argument("Invalid reasoning index");
            while (details.size() <= static_cast<std::size_t>(index)) details.push_back(Json::object());
            auto& target = details[index];
            for (const auto& [key, item] : part.items()) {
                if ((key == "text" || key == "data" || key == "signature" || key == "summary")
                    && item.is_string() && target.contains(key))
                    target[key].get_ref<std::string&>() += item.get<std::string>();
                else target[key] = item;
            }
        }
    }
    if (delta.contains("tool_calls") && !delta["tool_calls"].is_null()) {
        if (!delta["tool_calls"].is_array()) throw std::invalid_argument("Invalid tool calls");
        auto& calls = message_["tool_calls"];
        if (calls.is_null()) calls = Json::array();
        for (const auto& part : delta["tool_calls"]) {
            const auto index = part.at("index").get<int>();
            if (index < 0 || index >= 32) throw std::invalid_argument("Invalid tool index");
            while (calls.size() <= static_cast<std::size_t>(index)) calls.push_back(Json::object());
            auto& call = calls[index];
            for (const auto field : {"id", "type"}) {
                if (part.contains(field) && !part[field].is_null()) call[field] = part[field];
            }
            if (part.contains("function")) {
                for (const auto field : {"name", "arguments"}) {
                    const auto& function = part["function"];
                    if (!function.contains(field) || function[field].is_null()) continue;
                    auto& text = call["function"][field];
                    if (text.is_null()) text = "";
                    text.get_ref<std::string&>() += function[field].get<std::string>();
                    if (text.get_ref<std::string&>().size() > 16384)
                        throw std::invalid_argument("Tool arguments too large");
                }
            }
        }
    }
}

bool ChatCompletionsStreamDecoder::handle_event_data(std::string_view data) {
    if (data == "[DONE]") {
        done_ = true;
        return false;
    }
    Json value;
    const Json::json_pointer delta_pointer("/choices/0/delta");
    try {
        value = Json::parse(data);
        if (value.contains("usage")) usage_ = chat_token_usage(value);
        if (!value.contains("choices") || !value["choices"].is_array()) {
            if (protocol_error_.empty()) protocol_error_ = "Streaming event did not contain a choices array";
            return true;
        }
        const Json::json_pointer reason_pointer("/choices/0/finish_reason");
        if (collect_tool_calls_ && value.contains(reason_pointer) && value.at(reason_pointer).is_string())
            finish_reason_ = value.at(reason_pointer).get<std::string>();
        if (!value.contains(delta_pointer) || !value.at(delta_pointer).is_object()) return true;
        if (collect_tool_calls_) accumulate_delta(value.at(delta_pointer));
    } catch (const Json::parse_error&) {
        if (protocol_error_.empty()) protocol_error_ = "Streaming event contained malformed JSON";
        return true;
    } catch (const std::exception&) {
        if (protocol_error_.empty()) protocol_error_ = "Streaming event contained invalid tool data";
        return true;
    }
    // Sink exceptions belong to the caller, not to protocol validation.
    std::string error = process_response_object(value.at(delta_pointer), format_,
        [this](GenerationDeltaKind kind, std::string text) { emit(kind, std::move(text)); });
    if (protocol_error_.empty()) protocol_error_ = std::move(error);
    return true;
}

void ChatCompletionsStreamDecoder::emit(
    GenerationDeltaKind kind,
    std::string text) {
    if (kind == GenerationDeltaKind::reasoning) {
        received_reasoning_ = true;
    } else {
        received_answer_ = true;
    }
    (*on_delta_)(GenerationDelta{kind, std::move(text)});
}

GenerationResult decode_chat_completions_response(
    std::string_view body,
    ReasoningFormat format,
    const GenerationDeltaSink& on_delta,
    bool collect_tool_calls) {
    Json value;
    try {
        value = Json::parse(body);
    } catch (const Json::exception& error) {
        return {
            GenerationOutcome::protocol_error,
            "Inference server returned invalid JSON: "
                + std::string(error.what()),
        };
    }

    const Json::json_pointer message_pointer("/choices/0/message");
    if (!value.contains(message_pointer)
        || !value.at(message_pointer).is_object()) {
        return {
            GenerationOutcome::protocol_error,
            "Response did not contain choices[0].message",
        };
    }

    bool received_answer = false;
    const GenerationTokenUsage usage = chat_token_usage(value);
    const std::string protocol_error = process_response_object(
        value.at(message_pointer),
        format,
        [&on_delta, &received_answer](
            GenerationDeltaKind kind,
            std::string text) {
            if (kind == GenerationDeltaKind::answer) {
                received_answer = true;
            }
            on_delta(GenerationDelta{kind, std::move(text)});
        });

    if (!protocol_error.empty()) {
        return {GenerationOutcome::protocol_error, protocol_error};
    }
    const Json::json_pointer reason_pointer("/choices/0/finish_reason");
    const std::string finish_reason = collect_tool_calls && value.contains(reason_pointer)
            && value.at(reason_pointer).is_string()
        ? value.at(reason_pointer).get<std::string>() : std::string{};
    return tool_call_result(value.at(message_pointer), ProviderApi::chat_completions,
        received_answer, usage, collect_tool_calls,
        "Response completed without answer content", finish_reason);
}

} // namespace cha
