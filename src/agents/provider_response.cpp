#include "agents/provider_response.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
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

void normalize_newlines(std::string& text) {
    std::size_t index = 0;
    while ((index = text.find("\r\n", index)) != std::string::npos) {
        text.erase(index, 1);
    }
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
            (void)emit_field("reasoning", false);
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

ProviderStreamDecoder::ProviderStreamDecoder(
    ReasoningFormat format,
    const GenerationDeltaSink& on_delta)
    : format_(format),
      on_delta_(&on_delta) {
}

void ProviderStreamDecoder::consume(std::string_view bytes) {
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

StreamDecodeResult ProviderStreamDecoder::finish() {
    if (!pending_.empty()) {
        read_event(pending_);
        pending_.clear();
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

void ProviderStreamDecoder::read_event(std::string_view event) {
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

            if (data == "[DONE]") {
                done_ = true;
                return;
            }
            if (!data.empty()) {
                try {
                    const Json value = Json::parse(data);
                    if (value.contains("usage")) {
                        usage_ = chat_token_usage(value);
                    }
                    const Json::json_pointer choices_pointer("/choices");
                    const Json::json_pointer delta_pointer(
                        "/choices/0/delta");
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
                        protocol_error_ =
                            "Streaming event contained malformed JSON";
                    }
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }
}

void ProviderStreamDecoder::emit(GenerationDeltaKind kind, std::string text) {
    if (kind == GenerationDeltaKind::reasoning) {
        received_reasoning_ = true;
    } else {
        received_answer_ = true;
    }
    (*on_delta_)(GenerationDelta{kind, std::move(text)});
}

GenerationResult decode_provider_response(
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
