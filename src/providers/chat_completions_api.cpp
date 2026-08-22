#include "providers/chat_completions_api.h"

#include "util/json_serialization.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {

namespace {

using Json = nlohmann::json;

std::optional<std::size_t> token_count(
    const Json& usage,
    std::string_view field) {
    const auto value = usage.find(field);
    if (value == usage.end() || !value->is_number_unsigned()) {
        return std::nullopt;
    }
    const auto count = value->get<std::uint64_t>();
    if (count > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(count);
}

GenerationTokenUsage chat_token_usage(const Json& response) {
    const auto usage = response.find("usage");
    if (usage == response.end() || !usage->is_object()) {
        return {};
    }
    std::optional<std::size_t> cache_read_tokens;
    const auto details = usage->find("prompt_tokens_details");
    if (details != usage->end() && details->is_object()) {
        cache_read_tokens = token_count(*details, "cached_tokens");
    }
    if (!cache_read_tokens) {
        cache_read_tokens = token_count(*usage, "prompt_cache_hit_tokens");
    }
    return {
        .input_tokens = token_count(*usage, "prompt_tokens"),
        .output_tokens = token_count(*usage, "completion_tokens"),
        .cache_read_tokens = cache_read_tokens,
    };
}

std::string_view role_name(ModelRole role) {
    switch (role) {
    case ModelRole::system: return "system";
    case ModelRole::persona: return "user";
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
    std::string_view system_prompt) {
    Json messages = Json::array();
    for (const ModelMessage& message :
         project_model_context(input, system_prompt)) {
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
    if (!input.run.prompt_cache_key.empty()
        && config.cache_retention != CacheRetention::off
        && is_direct_openai_host(config.host)) {
        body["prompt_cache_key"] = input.run.prompt_cache_key;
    }

    return dump_json(body, "Model request");
}

ChatCompletionsStreamDecoder::ChatCompletionsStreamDecoder(
    ReasoningFormat format,
    const GenerationDeltaSink& on_delta)
    : format_(format),
      on_delta_(&on_delta) {
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
    if (!received_answer_) {
        return {{
            GenerationOutcome::protocol_error,
            "Streaming response completed without answer content",
            usage_,
        }, false};
    }
    return {{GenerationOutcome::completed, {}, usage_}, false};
}

bool ChatCompletionsStreamDecoder::handle_event_data(std::string_view data) {
    if (data == "[DONE]") {
        done_ = true;
        return false;
    }
    try {
        const Json value = Json::parse(data);
        if (value.contains("usage")) {
            usage_ = chat_token_usage(value);
        }
        const Json::json_pointer choices_pointer("/choices");
        const Json::json_pointer delta_pointer("/choices/0/delta");
        if (!value.contains(choices_pointer)
            || !value.at(choices_pointer).is_array()) {
            if (protocol_error_.empty()) {
                protocol_error_ =
                    "Streaming event did not contain a choices array";
            }
        } else if (value.contains(delta_pointer)
            && value.at(delta_pointer).is_object()) {
            std::string error = process_response_object(
                value.at(delta_pointer),
                format_,
                [this](GenerationDeltaKind kind, std::string text) {
                    emit(kind, std::move(text));
                });
            if (protocol_error_.empty()) {
                protocol_error_ = std::move(error);
            }
        }
    } catch (const Json::parse_error&) {
        if (protocol_error_.empty()) {
            protocol_error_ = "Streaming event contained malformed JSON";
        }
    }
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
    const GenerationDeltaSink& on_delta) {
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
    if (!received_answer) {
        return {
            GenerationOutcome::protocol_error,
            "Response completed without answer content",
        };
    }
    return {GenerationOutcome::completed, {}, usage};
}

} // namespace cha
