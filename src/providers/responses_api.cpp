#include "providers/responses_api.h"

#include "util/json_serialization.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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

GenerationTokenUsage responses_token_usage(const Json& response) {
    const auto usage = response.find("usage");
    if (usage == response.end() || !usage->is_object()) {
        return {};
    }
    std::optional<std::size_t> cache_read_tokens;
    std::optional<std::size_t> cache_write_tokens;
    const auto details = usage->find("input_tokens_details");
    if (details != usage->end() && details->is_object()) {
        cache_read_tokens = token_count(*details, "cached_tokens");
        cache_write_tokens = token_count(*details, "cache_write_tokens");
    }
    return {
        .input_tokens = token_count(*usage, "input_tokens"),
        .output_tokens = token_count(*usage, "output_tokens"),
        .cache_read_tokens = cache_read_tokens,
        .cache_write_tokens = cache_write_tokens,
    };
}

std::string_view input_role_name(ModelRole role) {
    switch (role) {
    case ModelRole::user: return "user";
    case ModelRole::assistant: return "assistant";
    case ModelRole::system:
        throw std::logic_error("System messages belong in instructions");
    }
    throw std::logic_error("Unknown model context role");
}

std::string first_string_field(const Json& object, std::string_view field) {
    const auto iterator = object.find(field);
    if (iterator == object.end() || !iterator->is_string()) {
        return {};
    }
    return iterator->get<std::string>();
}

std::string nested_string_field(
    const Json& object,
    std::string_view parent,
    std::string_view field) {
    const auto parent_iterator = object.find(parent);
    if (parent_iterator == object.end() || !parent_iterator->is_object()) {
        return {};
    }
    return first_string_field(*parent_iterator, field);
}

bool is_successful_completed_status(std::string_view status) {
    return status.empty() || status == "completed";
}

} // namespace

std::string build_responses_request_body(
    const GenerationRequest& input,
    const ModelBackendConfig& config,
    std::string_view system_prompt) {
    Json messages = Json::array();
    std::string instructions;
    for (const ModelMessage& message : project_model_context(input, system_prompt)) {
        if (message.role == ModelRole::system) {
            if (instructions.empty()) {
                instructions = message.content;
            }
            continue;
        }
        messages.push_back({
            {"role", input_role_name(message.role)},
            {"content", message.content},
        });
    }

    Json body{
        {"model", config.model},
        {"stream", config.stream},
        {"store", false},
        {"input", std::move(messages)},
    };
    if (config.temperature) {
        body["temperature"] = *config.temperature;
    }
    if (config.max_tokens) {
        body["max_output_tokens"] = std::max(*config.max_tokens, 16);
    }
    if (!instructions.empty()) {
        body["instructions"] = std::move(instructions);
    }
    if (!config.reasoning_effort.empty()) {
        body["reasoning"] = Json{{"effort", config.reasoning_effort}};
    }
    if (!input.run.prompt_cache_key.empty()
        && config.cache_retention != CacheRetention::off) {
        if (is_direct_openai_host(config.host)) {
            body["prompt_cache_key"] = input.run.prompt_cache_key;
            if (config.cache_retention == CacheRetention::long_) {
                body["prompt_cache_options"] = {
                    {"mode", "implicit"},
                    {"ttl", "30m"},
                };
            }
        } else if (is_openrouter_host(config.host)) {
            body["session_id"] = input.run.prompt_cache_key;
        }
    }

    const char* const web_search_type = is_openrouter_host(config.host)
        ? "openrouter:web_search"
        : "web_search";
    switch (config.web_search) {
    case WebSearchMode::off:
        break;
    case WebSearchMode::automatic:
        body["tools"] = Json::array({Json{{"type", web_search_type}}});
        body["tool_choice"] = "auto";
        break;
    case WebSearchMode::required:
        body["tools"] = Json::array({Json{{"type", web_search_type}}});
        body["tool_choice"] = "required";
        break;
    default:
        throw std::logic_error("Unknown web search mode");
    }

    return dump_json(body, "Model request");
}

ResponsesStreamDecoder::ResponsesStreamDecoder(const GenerationDeltaSink& on_delta)
    : on_delta_(&on_delta) {
}

void ResponsesStreamDecoder::consume(std::string_view bytes) {
    if (done_) {
        return;
    }
    framer_.consume(bytes, [this](std::string_view data) {
        return handle_event_json(data);
    });
}

StreamDecodeResult ResponsesStreamDecoder::finish() {
    if (!done_) {
        framer_.finish([this](std::string_view data) {
            return handle_event_json(data);
        });
    }

    if (!protocol_error_.empty()) {
        return {{GenerationOutcome::protocol_error, protocol_error_, usage_}, describe_response_};
    }
    if (!completed_successfully_) {
        return {{
            GenerationOutcome::protocol_error,
            received_answer_
                ? "Streaming response ended before response.completed"
                : "Streaming response was not valid Responses SSE",
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

bool ResponsesStreamDecoder::handle_event_json(std::string_view data) {
    if (done_) {
        return false;
    }

    Json value;
    try {
        value = Json::parse(data);
    } catch (const Json::parse_error&) {
        if (protocol_error_.empty()) {
            protocol_error_ = "Streaming event contained malformed JSON";
            describe_response_ = true;
        }
        return true;
    }

    const auto type_iterator = value.find("type");
    if (type_iterator == value.end() || !type_iterator->is_string()) {
        return true;
    }
    const std::string type = type_iterator->get<std::string>();

    if (type == "response.output_text.delta" || type == "response.refusal.delta") {
        const auto delta = value.find("delta");
        if (delta == value.end() || !delta->is_string()) {
            if (protocol_error_.empty()) {
                protocol_error_ =
                    "Responses event '" + type + "' did not contain a string delta";
                describe_response_ = true;
            }
            return true;
        }
        std::string text = delta->get<std::string>();
        if (!text.empty()) {
            emit_answer(std::move(text));
        }
        return true;
    }

    if (type == "response.completed") {
        done_ = true;
        const auto response = value.find("response");
        if (response != value.end() && response->is_object()) {
            usage_ = responses_token_usage(*response);
            const std::string status = first_string_field(*response, "status");
            if (!is_successful_completed_status(status)) {
                if (protocol_error_.empty()) {
                    protocol_error_ =
                        "Responses stream completed with status '" + status + "'";
                    describe_response_ = false;
                }
                return false;
            }
            const std::string error = nested_string_field(*response, "error", "message");
            if (!error.empty()) {
                if (protocol_error_.empty()) {
                    protocol_error_ = "Responses stream failed: " + error;
                    describe_response_ = false;
                }
                return false;
            }
        }
        if (protocol_error_.empty()) {
            completed_successfully_ = true;
        }
        return false;
    }

    if (type == "response.failed") {
        done_ = true;
        if (protocol_error_.empty()) {
            std::string error;
            const auto response = value.find("response");
            if (response != value.end() && response->is_object()) {
                error = nested_string_field(*response, "error", "message");
            }
            if (error.empty()) {
                error = nested_string_field(value, "error", "message");
            }
            if (error.empty()) {
                error = first_string_field(value, "message");
            }
            protocol_error_ = error.empty()
                ? "Responses stream failed"
                : "Responses stream failed: " + error;
            describe_response_ = false;
        }
        return false;
    }

    if (type == "response.incomplete") {
        done_ = true;
        if (protocol_error_.empty()) {
            std::string reason;
            const auto response = value.find("response");
            if (response != value.end() && response->is_object()) {
                const auto details = response->find("incomplete_details");
                if (details != response->end() && details->is_object()) {
                    reason = first_string_field(*details, "reason");
                }
            }
            if (reason.empty()) {
                reason = first_string_field(value, "reason");
            }
            protocol_error_ = reason.empty()
                ? "Responses stream ended incomplete"
                : "Responses stream ended incomplete: " + reason;
            describe_response_ = false;
        }
        return false;
    }

    if (type == "error") {
        done_ = true;
        if (protocol_error_.empty()) {
            const std::string message = first_string_field(value, "message");
            protocol_error_ = message.empty()
                ? "Responses stream error event"
                : "Responses stream error: " + message;
            describe_response_ = false;
        }
        return false;
    }

    // Web-search lifecycle, reasoning, item lifecycle, and other nonterminal
    // content events are private implementation details and are ignored.
    return true;
}

void ResponsesStreamDecoder::emit_answer(std::string text) {
    received_answer_ = true;
    (*on_delta_)(GenerationDelta{GenerationDeltaKind::answer, std::move(text)});
}

GenerationResult decode_responses_response(
    std::string_view body,
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

    if (!value.is_object()) {
        return {
            GenerationOutcome::protocol_error,
            "Responses body was not a JSON object",
        };
    }

    const auto top_error = value.find("error");
    if (top_error != value.end() && !top_error->is_null()) {
        std::string message;
        if (top_error->is_object()) {
            message = first_string_field(*top_error, "message");
        } else if (top_error->is_string()) {
            message = top_error->get<std::string>();
        }
        return {
            GenerationOutcome::protocol_error,
            message.empty()
                ? "Responses body contained an error"
                : "Responses body contained an error: " + message,
        };
    }

    const std::string status = first_string_field(value, "status");
    const GenerationTokenUsage usage = responses_token_usage(value);
    if (!status.empty() && status != "completed") {
        if (status == "incomplete") {
            std::string reason;
            const auto details = value.find("incomplete_details");
            if (details != value.end() && details->is_object()) {
                reason = first_string_field(*details, "reason");
            }
            return {
                GenerationOutcome::protocol_error,
                reason.empty()
                    ? "Responses body status was incomplete"
                    : "Responses body status was incomplete: " + reason,
            };
        }
        if (status == "failed") {
            const std::string message = nested_string_field(value, "error", "message");
            return {
                GenerationOutcome::protocol_error,
                message.empty()
                    ? "Responses body status was failed"
                    : "Responses body status was failed: " + message,
            };
        }
        return {
            GenerationOutcome::protocol_error,
            "Responses body status was '" + status + "'",
        };
    }

    const auto output = value.find("output");
    if (output == value.end() || !output->is_array()) {
        return {
            GenerationOutcome::protocol_error,
            "Responses body did not contain an output array",
        };
    }

    bool received_answer = false;
    for (const Json& item : *output) {
        if (!item.is_object()) {
            continue;
        }
        const std::string type = first_string_field(item, "type");
        if (type != "message") {
            continue;
        }
        const std::string role = first_string_field(item, "role");
        if (!role.empty() && role != "assistant") {
            continue;
        }
        const auto content = item.find("content");
        if (content == item.end() || !content->is_array()) {
            continue;
        }
        for (const Json& part : *content) {
            if (!part.is_object()) {
                continue;
            }
            const std::string part_type = first_string_field(part, "type");
            if (part_type == "output_text") {
                const auto text = part.find("text");
                if (text == part.end() || !text->is_string()) {
                    return {
                        GenerationOutcome::protocol_error,
                        "Responses output_text part did not contain string text",
                    };
                }
                std::string answer = text->get<std::string>();
                if (!answer.empty()) {
                    received_answer = true;
                    on_delta(GenerationDelta{
                        GenerationDeltaKind::answer,
                        std::move(answer),
                    });
                }
            } else if (part_type == "refusal") {
                // OpenAI uses "refusal" as the field name on refusal content parts.
                std::string refusal = first_string_field(part, "refusal");
                if (refusal.empty()) {
                    refusal = first_string_field(part, "text");
                }
                if (!refusal.empty()) {
                    received_answer = true;
                    on_delta(GenerationDelta{
                        GenerationDeltaKind::answer,
                        std::move(refusal),
                    });
                }
            }
        }
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
