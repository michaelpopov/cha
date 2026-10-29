#include "providers/google_api.h"

#include "providers/tool_calls.h"
#include "util/json_serialization.h"
#include "util/logging.h"

#include <stdexcept>
#include <utility>

namespace cha {
namespace {
using Json = nlohmann::json;

void update_count(const Json& usage, const char* field,
    std::optional<std::uint64_t>& count) {
    const auto value = usage.find(field);
    if (value != usage.end() && value->is_number_unsigned()) {
        count = value->get<std::uint64_t>();
    }
}
} // namespace

bool google_file_search_only_model(std::string_view model) {
    if (model.starts_with("models/")) model.remove_prefix(7);
    return model.starts_with("gemini-2.5-");
}

std::string build_google_request_body(
    const GenerationRequest& input, const ModelBackendConfig& config,
    std::string_view system_prompt, RequestTextSizes* text_sizes) {
    if (text_sizes) *text_sizes = {};
    Json body{{"contents", Json::array()}};
    for (const auto& message : project_model_context(input, system_prompt)) {
        const bool system = message.role == ModelRole::system;
        if (text_sizes) {
            (system ? text_sizes->system_prompt_bytes : text_sizes->conversation_bytes)
                += message.content.size();
        }
        if (message.content.empty()) continue;
        if (system) {
            auto& instruction = body["systemInstruction"];
            if (instruction.is_null()) instruction = {{"parts", Json::array()}};
            instruction["parts"].push_back({{"text", message.content}});
        } else {
            const auto role = message.role == ModelRole::assistant ? "model" : "user";
            auto& contents = body["contents"];
            if (contents.empty() || contents.back().at("role") != role) {
                contents.push_back({{"role", role}, {"parts", Json::array()}});
            }
            contents.back()["parts"].push_back({{"text", message.content}});
        }
    }
    Json generation{{"candidateCount", 1}};
    if (config.temperature) generation["temperature"] = *config.temperature;
    if (config.max_tokens) generation["maxOutputTokens"] = *config.max_tokens;
    body["generationConfig"] = std::move(generation);
    if (input.maintenance_tool) {
        add_maintenance_tools(body, ProviderApi::google);
    } else {
        const bool stores = input.folder_search
            && input.folder_search->service == FolderStoreService::google
            && !input.folder_search->remote_store_ids.empty();
        // The known 2.5 models cannot combine File Search and custom functions.
        // https://ai.google.dev/gemini-api/docs/generate-content/file-search
        // Unknown models keep requested functions and expose provider rejection.
        const bool file_search_only = stores && google_file_search_only_model(config.model);
        if (file_search_only && (input.web_search_tool || input.web_read_tool || input.summary_tool))
            log_warn("Omitting optional CHA custom functions: " + config.model
                + " does not support mixing them with File Search.");
        if (!file_search_only) {
            if (input.web_search_tool) add_web_search_tool(body, ProviderApi::google);
            if (input.web_read_tool) add_web_read_tool(body, ProviderApi::google);
            if (input.summary_tool) add_summary_tools(body, ProviderApi::google);
        }
        if (stores) {
            if (!body.contains("tools")) body["tools"] = Json::array();
            body["tools"].push_back({{"fileSearch", {
                {"fileSearchStoreNames", input.folder_search->remote_store_ids}}}});
            if (!file_search_only && (input.web_search_tool || input.web_read_tool || input.summary_tool)) {
                body["toolConfig"] = {{"includeServerSideToolInvocations", true},
                    {"functionCallingConfig", {{"mode", "VALIDATED"}}}};
            }
        }
    }
    if (input.include_tool_instructions) {
        update_tool_instructions(body, ProviderApi::google, text_sizes, input.summary_context);
    }
    return dump_json(body, "Model request");
}

GoogleStreamDecoder::GoogleStreamDecoder(const GenerationDeltaSink& on_delta,
    bool collect_tool_calls, std::size_t argument_limit, bool maintenance)
    : on_delta_(&on_delta), collect_tool_calls_(collect_tool_calls),
      argument_limit_(argument_limit), maintenance_(maintenance) {}

void GoogleStreamDecoder::consume(std::string_view bytes) {
    framer_.consume(bytes, [this](std::string_view data) { return process_json(data); });
}

bool GoogleStreamDecoder::process_json(std::string_view data) {
    // Do not stop at finishReason: usage and signature-only parts can follow it.
    try {
        const auto value = Json::parse(data);
        if (!value.is_object()) throw std::invalid_argument("Invalid Google response");
        if (const auto usage = value.find("usageMetadata"); usage != value.end()) {
            if (!usage->is_object()) throw std::invalid_argument("Invalid Google usage");
            update_count(*usage, "promptTokenCount", usage_.input_tokens);
            update_count(*usage, "candidatesTokenCount", candidate_tokens_);
            update_count(*usage, "thoughtsTokenCount", thought_tokens_);
            update_count(*usage, "cachedContentTokenCount", usage_.cache_read_tokens);
        }
        if (value.contains("error")) {
            protocol_error_ = value.at("error").value("message", std::string{}).substr(0, 512);
            if (protocol_error_.empty()) protocol_error_ = "Google returned an error";
            return true;
        }
        if (!protocol_error_.empty()) return true;
        if (const auto feedback = value.find("promptFeedback"); feedback != value.end()) {
            const auto block = feedback->value("blockReason", "");
            if (!block.empty() && block != "BLOCK_REASON_UNSPECIFIED") {
                prompt_blocked_ = true;
                refused_ = true;
            }
        }
        const auto candidates = value.find("candidates");
        if (candidates == value.end()) return true;
        if (!candidates->is_array()) throw std::invalid_argument("Invalid Google candidates");
        for (const auto& candidate : *candidates) {
            if (candidate.value("index", 0) != 0) continue;
            if (const auto content = candidate.find("content"); content != candidate.end()) {
                if (!content->is_object() || content->value("role", "model") != "model") {
                    throw std::invalid_argument("Invalid Google model content");
                }
                const auto parts = content->value("parts", Json::array());
                if (!parts.is_array()) throw std::invalid_argument("Invalid Google parts");
                for (const auto& part : parts) {
                    if (!part.is_object()) throw std::invalid_argument("Invalid Google part");
                    if (const auto text = part.find("text"); text != part.end()) {
                        const auto& delta = text->get_ref<const std::string&>();
                        const bool thought = part.value("thought", false);
                        if (!delta.empty()) {
                            if (!thought) received_answer_ = true;
                            (*on_delta_)({thought ? GenerationDeltaKind::reasoning
                                : GenerationDeltaKind::answer, delta});
                        }
                    }
                    content_["parts"].push_back(part);
                }
            }
            const auto reason = candidate.value("finishReason", "");
            if (reason.empty()) continue;
            if (reason == "STOP") finish_reason_ = "stop";
            else if (reason == "MAX_TOKENS") finish_reason_ = "length";
            else if (reason == "SAFETY" || reason == "RECITATION" || reason == "BLOCKLIST"
                || reason == "PROHIBITED_CONTENT" || reason == "SPII") {
                refused_ = true;
                finish_reason_ = "content_filter";
            } else {
                protocol_error_ = "Unsupported Google finishReason: " + reason;
            }
        }
    } catch (const Json::exception&) {
        if (protocol_error_.empty()) protocol_error_ = "Malformed Google response JSON";
    } catch (const std::invalid_argument& error) {
        if (protocol_error_.empty()) protocol_error_ = error.what();
    }
    return true;
}

StreamDecodeResult GoogleStreamDecoder::finish() {
    framer_.finish([this](std::string_view data) { return process_json(data); });
    if (candidate_tokens_) usage_.output_tokens = *candidate_tokens_ + thought_tokens_.value_or(0);
    GenerationResult result;
    if (!protocol_error_.empty()) {
        result = {GenerationOutcome::protocol_error, protocol_error_, usage_};
    } else if (prompt_blocked_) {
        result = {GenerationOutcome::protocol_error, "Google blocked the prompt", usage_};
    } else if (finish_reason_.empty()) {
        result = {GenerationOutcome::protocol_error, "Google response ended before a terminal status", usage_};
    } else {
        result = tool_call_result(content_, ProviderApi::google, received_answer_, usage_,
            collect_tool_calls_, "Response completed without answer content", finish_reason_,
            argument_limit_, maintenance_);
    }
    result.finish_reason = finish_reason_;
    result.refused = refused_;
    return {std::move(result), false};
}

GenerationResult decode_google_response(std::string_view body,
    const GenerationDeltaSink& on_delta, bool collect_tool_calls,
    std::size_t argument_limit, bool maintenance) {
    GoogleStreamDecoder decoder(on_delta, collect_tool_calls, argument_limit, maintenance);
    decoder.process_json(body);
    return decoder.finish().result;
}

} // namespace cha
