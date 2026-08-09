#include "agents/responses_api.h"

#include "util/json_serialization.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {

namespace {

using Json = nlohmann::json;

void normalize_newlines(std::string& text) {
    std::size_t index = 0;
    while ((index = text.find("\r\n", index)) != std::string::npos) {
        text.erase(index, 1);
    }
}

std::string_view input_role_name(ModelRole role) {
    switch (role) {
    case ModelRole::persona: return "user";
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
        {"temperature", config.temperature},
    };
    if (!instructions.empty()) {
        body["instructions"] = std::move(instructions);
    }
    if (!config.reasoning_effort.empty()) {
        body["reasoning"] = Json{{"effort", config.reasoning_effort}};
    }

    switch (config.web_search) {
    case WebSearchMode::off:
        break;
    case WebSearchMode::automatic:
        body["tools"] = Json::array({Json{{"type", "web_search"}}});
        body["tool_choice"] = "auto";
        break;
    case WebSearchMode::required:
        body["tools"] = Json::array({Json{{"type", "web_search"}}});
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
    pending_.append(bytes);
    normalize_newlines(pending_);

    std::size_t event_end = 0;
    while ((event_end = pending_.find("\n\n")) != std::string::npos) {
        const std::string event = pending_.substr(0, event_end);
        pending_.erase(0, event_end + 2);
        read_event(event);
        if (done_) {
            pending_.clear();
            return;
        }
    }
}

StreamDecodeResult ResponsesStreamDecoder::finish() {
    if (!pending_.empty()) {
        read_event(pending_);
        pending_.clear();
    }

    if (!protocol_error_.empty()) {
        return {{GenerationOutcome::protocol_error, protocol_error_}, describe_response_};
    }
    if (!completed_successfully_) {
        return {{
            GenerationOutcome::protocol_error,
            received_answer_
                ? "Streaming response ended before response.completed"
                : "Streaming response was not valid Responses SSE",
        }, true};
    }
    if (!received_answer_) {
        return {{
            GenerationOutcome::protocol_error,
            "Streaming response completed without answer content",
        }, false};
    }
    return {{GenerationOutcome::completed, {}}, false};
}

void ResponsesStreamDecoder::read_event(std::string_view event) {
    std::size_t line_start = 0;
    while (line_start <= event.size()) {
        const std::size_t line_end = event.find('\n', line_start);
        const std::string_view line = event.substr(
            line_start,
            line_end == std::string_view::npos
                ? event.size() - line_start
                : line_end - line_start);

        if (line.starts_with("data:")) {
            std::string_view data = line.substr(5);
            while (!data.empty() && data.front() == ' ') {
                data.remove_prefix(1);
            }
            if (!data.empty()) {
                handle_event_json(data);
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }
}

void ResponsesStreamDecoder::handle_event_json(std::string_view data) {
    if (done_) {
        return;
    }

    Json value;
    try {
        value = Json::parse(data);
    } catch (const Json::parse_error&) {
        if (protocol_error_.empty()) {
            protocol_error_ = "Streaming event contained malformed JSON";
            describe_response_ = true;
        }
        return;
    }

    const auto type_iterator = value.find("type");
    if (type_iterator == value.end() || !type_iterator->is_string()) {
        return;
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
            return;
        }
        std::string text = delta->get<std::string>();
        if (!text.empty()) {
            emit_answer(std::move(text));
        }
        return;
    }

    if (type == "response.completed") {
        done_ = true;
        const auto response = value.find("response");
        if (response != value.end() && response->is_object()) {
            const std::string status = first_string_field(*response, "status");
            if (!is_successful_completed_status(status)) {
                if (protocol_error_.empty()) {
                    protocol_error_ =
                        "Responses stream completed with status '" + status + "'";
                    describe_response_ = false;
                }
                return;
            }
            const std::string error = nested_string_field(*response, "error", "message");
            if (!error.empty()) {
                if (protocol_error_.empty()) {
                    protocol_error_ = "Responses stream failed: " + error;
                    describe_response_ = false;
                }
                return;
            }
        }
        if (protocol_error_.empty()) {
            completed_successfully_ = true;
        }
        return;
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
        return;
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
        return;
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
        return;
    }

    // Web-search lifecycle, reasoning, item lifecycle, and other nonterminal
    // content events are private implementation details and are ignored.
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
    return {GenerationOutcome::completed, {}};
}

} // namespace cha
