#include "notes/notes_service_internal.h"

#include "providers/gemini_token_cost.h"
#include "providers/openai_token_cost.h"
#include "providers/web_search.h"
#include "session/session_markdown.h"
#include "storage/not_found_error.h"
#include "storage/note_storage.h"
#include "util/base64.h"
#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"
#include "workspace/builtins.h"

#include <algorithm>
#include <iomanip>
#include <initializer_list>
#include <locale>
#include <sstream>
#include <thread>

namespace cha::app {

void NoteSummaryCost::add(std::optional<double> estimate) {
    if (estimate) {
        usd += *estimate;
        has_estimate = true;
    } else {
        incomplete = true;
    }
}

void NoteSummaryCost::merge(const NoteSummaryCost& other) {
    usd += other.usd;
    openai |= other.openai;
    openai_flex |= other.openai_flex;
    openai_other_or_unknown_tier |= other.openai_other_or_unknown_tier;
    gemini_flex |= other.gemini_flex;
    incomplete |= other.incomplete;
    has_estimate |= other.has_estimate;
}

namespace {
constexpr std::size_t note_compression_min_bytes = 4 * 1024;

ProviderSelection note_editing_provider(const Workspace& workspace) {
    const auto* provider = workspace.find_provider(workspace.notes().compression_provider_id);
    if (!provider) {
        throw ApplicationError(ErrorCode::invalid_argument,
            "Select a compression provider in Notes settings.");
    }
    const auto& effort = workspace.notes().compression_reasoning_effort;
    if (effort != "none"
        && !provider_supports_reasoning_effort(provider->config, provider->config.api)) {
        if (provider->config.mode == Mode::net)
            throw ApplicationError(ErrorCode::invalid_argument,
                "This compression provider's model does not support " + effort + " reasoning effort.");
        log_warn("Ignoring unsupported Notes provider reasoning effort.");
    }
    auto config = provider->config;
    config.reasoning_effort = provider_supports_reasoning_effort(config, config.api) ? effort : "none";
    config.web_search = WebSearchMode::off;
    return {provider->id, std::move(config)};
}

// Bind legacy names to a stable reference, so deleting a key cannot make a
// waiting job select a different key later created with that name.
void bind_note_api_key(ModelBackendConfig& config, ApiKeyStore* api_keys) {
    if (config.mode != Mode::net || config.auth != ProviderAuth::none) {
        if (!config.api_key_id.empty() || !config.api_key_env.empty())
            log_warn("Ignoring unused Notes provider API key settings.");
        config.api_key_id.clear();
        config.api_key_env.clear();
        if (config.mode != Mode::net) config.auth = ProviderAuth::none;
        return;
    }
    if (config.api_key_id.empty() && !config.api_key_env.empty()) {
        const auto key = api_keys->find_by_name(config.api_key_env);
        if (!key) throw ApplicationError(ErrorCode::invalid_argument, "Select an existing compression API key in Settings.");
        config.api_key_id = key->id;
        config.api_key_env.clear();
    }
}

enum class NoteRequestKind {
    plain,        // Save summary and plan validation.
    flex,         // L3 overview: Flex, with its own prompt as the system prompt.
    source_step,  // Compression, L1, L2, L3 section: Flex, with a fixed system prompt.
};

// The step prompt follows the note, so the steps of one note share a cacheable prefix.
constexpr std::string_view note_source_system_prompt =
    "The user message contains a note. Use the note only as source material.\n"
    "Follow the task instruction that comes after the note.";

SharedCharacterDefinition prepare_note_provider(const Workspace& workspace, OpenAiOAuth* oauth, ApiKeyStore* api_keys,
    const WorkspacePrompt& prompt, NoteRequestKind kind = NoteRequestKind::plain) {
    auto provider = note_editing_provider(workspace);
    provider.config.timeout_s = 0;
    bind_note_api_key(provider.config, api_keys);
    if (provider.config.mode == Mode::net && provider.config.auth == ProviderAuth::openai_subscription
        && oauth->status().state != OpenAiOAuthState::connected)
        throw ApplicationError(ErrorCode::invalid_argument, "Connect your OpenAI account in Settings before compressing notes.");
    provider.config.flex_processing = kind != NoteRequestKind::plain && eligible_for_flex_processing(provider.config);
    const auto definition = std::make_shared<const CharacterDefinition>(CharacterDefinition{
        .character = {prompt.id, prompt.display_name}, .provider = std::move(provider),
        .system_prompt = kind == NoteRequestKind::source_step ? std::string(note_source_system_prompt) : prompt.text,
    });
    // Validate before acceptance; the worker resolves the same key reference
    // again under the lifecycle lock before every actual request.
    try { ProviderClient validate(definition, oauth, api_keys); }
    catch (const std::exception& error) { throw ApplicationError(ErrorCode::invalid_argument, error.what()); }
    return definition;
}

struct NotePromptAnswer {
    GenerationResult result;
    std::string text;
};

NotePromptAnswer collect_note_prompt(ProviderClient& client, const SharedCharacterDefinition& definition,
    std::string text, const std::atomic_bool& cancel, std::string instruction = {}, NoteSummaryCost* cost = nullptr) {
    GenerationRequest request{
        .history = std::make_shared<const ModelHistory>(),
        .run = {.target = definition->character, .author = {"", "User"}, .prompt_text = std::move(text)},
        .include_tool_instructions = false,
        .step_instruction = std::move(instruction),
    };
    NotePromptAnswer answer;
    const auto& config = definition->provider.config;
    const bool openai = cost && uses_openai_api_key(config);
    const bool gemini_flex = cost && config.api == ProviderApi::google && config.flex_processing;
    auto payload = client.prepare(request);
    if (cost) {
        cost->openai |= openai;
        cost->gemini_flex |= gemini_flex;
    }
    try {
        answer.result = client.perform(std::move(payload), [&answer](GenerationDelta delta) {
            if (delta.kind == GenerationDeltaKind::answer) answer.text += delta.text;
        }, cancel);
    } catch (...) {
        if (openai) cost->openai_other_or_unknown_tier = true;
        if (openai || gemini_flex) cost->add(std::nullopt);
        throw;
    }
    if (openai) {
        if (!answer.result.request_not_processed) {
            if (answer.result.service_tier == "flex") cost->openai_flex = true;
            else cost->openai_other_or_unknown_tier = true;
        }
        cost->add(estimate_openai_token_cost(config, answer.result));
    }
    if (gemini_flex) cost->add(estimate_gemini_flex_token_cost(config, answer.result));
    return answer;
}

NotePromptAnswer run_note_prompt(const SharedCharacterDefinition& definition, std::string text,
    OpenAiOAuth* oauth, ApiKeyStore* api_keys, const std::atomic_bool& cancel) {
    std::unique_ptr<ProviderClient> client;
    try {
        client = std::make_unique<ProviderClient>(definition, oauth, api_keys);
    } catch (const std::exception& error) {
        throw ApplicationError(ErrorCode::invalid_argument, error.what());
    }
    return collect_note_prompt(*client, definition, std::move(text), cancel);
}

bool is_youtube_url(std::string_view url) {
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    const std::string owned(url);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, owned.c_str(), 0) != CURLUE_OK) return false;
    const auto hostname = [&] {
        char* raw_host = nullptr;
        if (curl_url_get(parsed.get(), CURLUPART_HOST, &raw_host, 0) != CURLUE_OK) return std::string{};
        const std::unique_ptr<char, decltype(&curl_free)> host(raw_host, curl_free);
        auto name = fold_ascii(host.get());
        if (name.ends_with('.')) name.pop_back();
        return name;
    };
    auto name = hostname();
    if (name == "safelinks.protection.outlook.com" || name.ends_with(".safelinks.protection.outlook.com")) {
        char* raw_query = nullptr;
        if (curl_url_get(parsed.get(), CURLUPART_QUERY, &raw_query, 0) != CURLUE_OK) return false;
        const std::unique_ptr<char, decltype(&curl_free)> query(raw_query, curl_free);
        auto parameters = std::string_view(query.get());
        while (!parameters.empty()) {
            const auto end = parameters.find('&');
            const auto parameter = parameters.substr(0, end);
            if (parameter.starts_with("url=")) {
                // Split before decoding so escaped '&' characters stay inside the target URL.
                const std::string encoded(parameter.substr(4));
                int length = 0;
                const std::unique_ptr<char, decltype(&curl_free)> target(
                    curl_easy_unescape(nullptr, encoded.c_str(), static_cast<int>(encoded.size()), &length), curl_free);
                if (!target || std::string_view(target.get(), static_cast<std::size_t>(length)).find('\0') != std::string_view::npos)
                    return false;
                if (curl_url_set(parsed.get(), CURLUPART_URL, target.get(), 0) != CURLUE_OK) return false;
                name = hostname();
                break;
            }
            if (end == std::string_view::npos) break;
            parameters.remove_prefix(end + 1);
        }
    }
    return name == "youtube.com" || name.ends_with(".youtube.com")
        || name == "youtu.be" || name == "www.youtu.be";
}

std::string transcript_title(std::string_view text) {
    text = trim_view(text);
    const auto first_line = text.substr(0, text.find('\n'));
    return first_line.starts_with("# ") ? std::string(trim_view(first_line.substr(2))) : std::string{};
}

// A lightweight Markdown heuristic: soft line breaks belong to the same paragraph.
// Leave headings, list blocks, and fenced/indented code out of the length check.
bool has_long_page_paragraph(std::string_view markdown) {
    constexpr std::size_t limit = 2000;
    std::size_t characters = 0, fence_size = 0;
    char fence = 0;
    bool list = false;
    while (!markdown.empty()) {
        const auto end = markdown.find('\n');
        const auto raw = markdown.substr(0, end);
        markdown = end == std::string_view::npos ? std::string_view{} : markdown.substr(end + 1);
        const auto line = trim_view(raw);
        if (fence) {
            const auto end_fence = line.find_first_not_of(fence);
            const auto count = end_fence == std::string_view::npos ? line.size() : end_fence;
            if (count >= fence_size && trim_view(line.substr(count)).empty()) fence = 0;
            continue;
        }
        if (line.starts_with("```") || line.starts_with("~~~")) {
            if (characters > limit) return true;
            fence = line.front();
            fence_size = line.find_first_not_of(fence);
            if (fence_size == std::string_view::npos) fence_size = line.size();
            characters = 0;
            continue;
        }
        if (line.empty()) {
            if (characters > limit) return true;
            characters = 0;
            list = false;
            continue;
        }
        // A setext underline makes the preceding text a heading.
        if (line.find_first_not_of('=') == std::string_view::npos
            || line.find_first_not_of('-') == std::string_view::npos) {
            characters = 0;
            continue;
        }
        const auto hashes = line.find_first_not_of('#');
        const bool heading = line.front() == '#' && hashes <= 6
            && hashes < line.size() && is_space(line[hashes]);
        const auto digits = line.find_first_not_of("0123456789");
        const bool ordered = digits >= 1 && digits <= 9 && digits + 1 < line.size()
            && (line[digits] == '.' || line[digits] == ')') && is_space(line[digits + 1]);
        const bool bullet = line.size() > 1 && (line.front() == '-' || line.front() == '+' || line.front() == '*')
            && is_space(line[1]);
        list = list || ordered || bullet;
        if (heading || list || raw.starts_with("    ") || raw.starts_with('\t')) {
            if (characters > limit) return true;
            characters = 0;
            continue;
        }
        if (characters) ++characters; // The space represented by a soft line break.
        for (const unsigned char c : line) if ((c & 0xc0) != 0x80) ++characters;
    }
    return characters > limit;
}

std::string format_note_page(const Workspace& workspace, std::string text, std::string& title,
    bool youtube, OpenAiOAuth* oauth, ApiKeyStore* api_keys, const std::atomic_bool& cancel) {
    title = trim_view(title);
    if (title.empty()) title = transcript_title(text);
    if (!title.empty() && transcript_title(text) != title) text = "# " + title + "\n\n" + text;
    const auto& prompt = *workspace.find_prompt(youtube ? "youtube-transcript" : "page-formatting");
    const auto definition = std::make_shared<const CharacterDefinition>(CharacterDefinition{
        .character = {prompt.id, prompt.display_name},
        .provider = note_editing_provider(workspace),
        .system_prompt = prompt.text,
    });
    auto answer = run_note_prompt(definition, std::move(text), oauth, api_keys, cancel);
    const auto& result = answer.result;
    if (cancel.load() || result.outcome == GenerationOutcome::cancelled) {
        throw ApplicationError(ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    if (result.refused) {
        throw ApplicationError(ErrorCode::internal_error,
            youtube ? "The provider refused to format this transcript." : "The provider refused to format this page.");
    }
    if (result.outcome != GenerationOutcome::completed) {
        throw ApplicationError(ErrorCode::internal_error, result.message.empty()
            ? "The compression provider returned an error." : result.message);
    }
    if (result.finish_reason == "length") {
        throw ApplicationError(ErrorCode::internal_error, youtube
            ? "The transcript is too long to format." : "The page is too long to format.");
    }
    if (result.finish_reason == "content_filter") {
        throw ApplicationError(ErrorCode::internal_error,
            youtube ? "The provider returned an incomplete transcript." : "The provider returned an incomplete page.");
    }
    if (answer.text.find_first_not_of(" \t\r\n") == std::string::npos) {
        throw ApplicationError(ErrorCode::internal_error,
            youtube ? "Could not format the YouTube transcript. Try again." : "Could not format the page. Try again.");
    }
    const auto formatted_title = transcript_title(answer.text);
    if (title.empty()) title = formatted_title;
    if (!title.empty()) {
        auto body = trim_view(answer.text);
        if (!formatted_title.empty()) {
            const auto end = body.find('\n');
            body = end == std::string_view::npos ? std::string_view{} : trim_view(body.substr(end + 1));
        }
        answer.text = "# " + title + "\n\n" + std::string(body);
    }
    if (answer.text.size() > note_document_byte_limit) {
        throw ApplicationError(ErrorCode::body_too_large, youtube
            ? "The transcript is too large to import." : "The page is too large to import.");
    }
    return std::move(answer.text);
}

std::string compression_target(std::int64_t id, std::string_view name) {
    return "“" + std::string(name) + "” (note " + std::to_string(id) + ")";
}
} // namespace

namespace {
std::string summary_target_label(const FullSessionId& identity, std::string_view label) {
    return "“" + std::string(label) + "” (" + identity.forum_id + "/" + identity.session_id + ")";
}

constexpr std::string_view summary_cancelled = "Summary generation was cancelled.";
constexpr std::string_view summary_start_failed = "Could not start summary generation. Try again.";
constexpr std::string_view missing_summary_target = "That note or folder was not found.";

struct AnswerMessages {
    std::string_view refused;
    std::string_view context_window;
    std::string_view incomplete;
    std::string_view no_text;
    std::string_view too_large;
};

void require_answer(const NotePromptAnswer& answer, const std::atomic_bool& cancel,
    const AnswerMessages& messages) {
    const auto& result = answer.result;
    if (cancel.load() || result.outcome == GenerationOutcome::cancelled)
        throw ApplicationError(ErrorCode::operation_cancelled, std::string(summary_cancelled));
    if (result.refused) throw ApplicationError(ErrorCode::internal_error, std::string(messages.refused));
    if (result.outcome != GenerationOutcome::completed) {
        if (result.message == "Prompt exceeds the model's context window.")
            throw ApplicationError(ErrorCode::invalid_argument, std::string(messages.context_window));
        const std::initializer_list<std::string_view> safe_reasons{
            "HTTP request failed: idle timeout reached", "HTTP request failed: Timeout was reached",
            "Provider quota or billing limit exceeded.", "Provider authentication or permission was rejected.",
            "Provider rate limit exceeded.",
        };
        const bool safe = std::find(safe_reasons.begin(), safe_reasons.end(), result.message) != safe_reasons.end();
        throw ApplicationError(ErrorCode::internal_error, safe ? result.message
            : "The provider request failed. Check the connection and provider settings, then try again.");
    }
    if (!result.finish_reason.empty() && result.finish_reason != "stop" && result.finish_reason != "end_turn")
        throw ApplicationError(ErrorCode::internal_error, std::string(messages.incomplete));
    if (trim_view(answer.text).empty())
        throw ApplicationError(ErrorCode::internal_error, std::string(messages.no_text));
    if (answer.text.size() > note_document_byte_limit)
        throw ApplicationError(ErrorCode::body_too_large, std::string(messages.too_large));
}

bool folder_summary_part(SummaryPart part) {
    return part == SummaryPart::l3 || part == SummaryPart::l3_overview;
}

std::string_view summary_part_label(SummaryPart part) {
    switch (part) {
    case SummaryPart::l1: return "L1";
    case SummaryPart::l2: return "L2";
    case SummaryPart::l3_section: return "L3 section";
    case SummaryPart::l3_overview: return "L3 overview";
    case SummaryPart::l3: return "Summary note";
    }
    throw ApplicationError(ErrorCode::invalid_argument);
}

std::string note_summary_target(std::int64_t id, std::string_view name, bool folder) {
    if (name.empty()) return std::string(folder ? "folder " : "note ") + std::to_string(id);
    if (!folder) return compression_target(id, name);
    return "“" + std::string(name) + "” (folder " + std::to_string(id) + ")";
}

void record_note_summary_failure(WorkspaceConfigStore& store, SummaryPart part,
    std::string_view target, std::string_view reason) noexcept {
    try {
        std::string message = part == SummaryPart::l3 ? "Could not save the " : "Could not make ";
        if (part == SummaryPart::l3_section || part == SummaryPart::l3_overview) message += "the ";
        message.append(summary_part_label(part)).append(" for ").append(target).append(": ").append(reason);
        if (!message.ends_with('.')) message += '.';
        if (part == SummaryPart::l3_section || part == SummaryPart::l3_overview)
            message += " The Summary note was not changed.";
        store.append_event(EventSeverity::error, message);
    } catch (...) { log_warn("Could not record note summary outcome."); }
}

void require_summary_target(const NoteItem& item, bool folder) {
    if (folder) {
        if (item.kind != NoteItemKind::folder)
            throw ApplicationError(ErrorCode::invalid_argument, "That item is not a folder.");
        if (item.forum_id) throw ApplicationError(ErrorCode::invalid_argument, "This folder is managed by CHA.");
    } else if (item.kind != NoteItemKind::note || item.is_summary) {
        throw ApplicationError(ErrorCode::invalid_argument, "That item is not a note.");
    }
}

std::string_view summary_prompt(SummaryPart part) {
    switch (part) {
    case SummaryPart::l1: return "summary-l1";
    case SummaryPart::l2: return "summary-l2";
    case SummaryPart::l3_section: return "summary-l3-section";
    case SummaryPart::l3_overview: return "summary-l3-overview";
    default: throw ApplicationError(ErrorCode::invalid_argument);
    }
}

}

void NotesService::set_note_compression_changed(NoteAudioChanged callback) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->note_compression_changed = std::move(callback);
}

std::optional<NoteText> NotesService::Impl::compression_source_locked(std::int64_t id, bool skip_ineligible) {
    NoteText note;
    try {
        const auto items = store->list_note_items();
        const auto item = std::find_if(items.begin(), items.end(), [id](const auto& item) { return item.id == id; });
        if (skip_ineligible && (item == items.end() || item->kind != NoteItemKind::note
            || item->is_summary || item->has_compressed || compressions.contains(id))) return std::nullopt;
        if (item != items.end() && item->kind != NoteItemKind::note)
            throw NoteError(NoteFailure::invalid, "That item is not a note.");
        if (item != items.end() && item->is_summary)
            throw NoteError(NoteFailure::invalid, "Session summaries cannot be compressed.");
        if (compressions.contains(id))
            throw ApplicationError(ErrorCode::invalid_argument, "Compression is already running for this note.");
        note = store->get_note_text(id);
    } catch (const NoteError& error) { rethrow_note(error); }
    if (note.text.size() < note_compression_min_bytes) {
        if (skip_ineligible) return std::nullopt;
        throw ApplicationError(ErrorCode::invalid_argument, "This note is under 4 KiB and does not need compression.");
    }
    if (note.text.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (skip_ineligible) return std::nullopt;
        throw ApplicationError(ErrorCode::invalid_argument, "This note has no text to compress.");
    }
    return note;
}

NotesService::Impl::CompressionResult NotesService::Impl::run_compression(
    const std::shared_ptr<CompressionControl>& control, const SharedCharacterDefinition& definition,
    const std::string& instruction, const NoteText& note, std::uint64_t epoch, std::atomic_bool& cancel,
    bool serialized, NoteSummaryCost* cost) {
    using Kind = CompressionResult::Kind;
    const auto target = compression_target(note.id, control->name);
    const auto discarded = "Compression discarded for " + target
        + ": the saved text changed. Compress again to use the current text.";
    auto severity = EventSeverity::error;
    std::string reason;
    try {
        // The separate Compress action runs one note at a time; summary lines do not wait.
        std::unique_lock serial(compression_mutex, std::defer_lock);
        while (serialized && !cancel.load() && !serial.try_lock_for(std::chrono::milliseconds(100))) {}
        NotePromptAnswer answer;
        for (unsigned retries = 0;; ++retries) {
            std::unique_ptr<ProviderClient> client;
            {
                const std::lock_guard lifecycle(lifecycle_mutex);
                const auto found = compressions.find(note.id);
                if (found == compressions.end() || found->second != control) return {Kind::cancelled};
                require_admitted(epoch);
                if (cancel.load()) throw ApplicationError(ErrorCode::operation_cancelled, "Cancelled.");
                if (store->get_note_text(note.id).text != note.text) {
                    retire_compression(note.id, control, epoch, EventSeverity::warning, discarded);
                    return {Kind::discarded};
                }
                client = std::make_unique<ProviderClient>(definition, openai_auth, api_keys,
                    settings.test_hooks.note_summary_transport);
            }
            answer = collect_note_prompt(*client, definition, note.text, cancel, instruction, cost);
            if (!answer.result.retryable || cancel.load() || retries == 3) break;
            const auto seconds = 10 << retries;
            if (settings.test_hooks.note_compression_wait) settings.test_hooks.note_compression_wait(seconds);
            else for (int i = 0; i < seconds * 10 && !cancel.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto& result = answer.result;
        const std::lock_guard lifecycle(lifecycle_mutex);
        const auto found = compressions.find(note.id);
        if (found == compressions.end() || found->second != control) return {Kind::cancelled};
        require_admitted(epoch);
        if (cancel.load() || result.outcome == GenerationOutcome::cancelled)
            throw ApplicationError(ErrorCode::operation_cancelled, "Cancelled.");
        if (result.refused) throw ApplicationError(ErrorCode::internal_error, "The provider refused to compress this note.");
        if (result.outcome != GenerationOutcome::completed) {
            // Provider diagnostics can contain a raw response or echoed input.
            // Persist only a safe, actionable reason in Events.
            const std::initializer_list<std::string_view> safe_reasons{
                "HTTP request failed: idle timeout reached", "HTTP request failed: Timeout was reached",
                "Provider quota or billing limit exceeded.", "Provider authentication or permission was rejected.",
                "Provider rate limit exceeded.", "Prompt exceeds the model's context window.",
            };
            const bool safe = std::find(safe_reasons.begin(), safe_reasons.end(), result.message) != safe_reasons.end();
            throw ApplicationError(ErrorCode::internal_error, safe ? result.message
                : "The provider request failed. Check the connection and provider settings, then try again.");
        }
        if (result.finish_reason == "length" || result.finish_reason == "content_filter")
            throw ApplicationError(ErrorCode::internal_error, "The provider returned incomplete compressed text.");
        if (store->commit_compressed_note(note.id, note.text, answer.text,
                "Compressed version ready for " + target + ".")) {
            retire_compression(note.id, control, epoch, EventSeverity::success, "");
            return {Kind::saved};
        }
        retire_compression(note.id, control, epoch, EventSeverity::warning, discarded);
        return {Kind::discarded};
    } catch (const ApplicationError& error) {
        if (cancel.load() || error.code == ErrorCode::operation_cancelled
            || error.code == ErrorCode::server_stopping || error.code == ErrorCode::application_unavailable) {
            severity = EventSeverity::info;
        } else reason = error.what();
    } catch (const NoteError& error) {
        reason = error.what();
    } catch (const std::exception&) {
        reason = "Check the provider settings and try again.";
    } catch (...) { reason = "Try again."; }
    const bool cancelled = severity == EventSeverity::info;
    const std::lock_guard lifecycle(lifecycle_mutex);
    retire_compression(note.id, control, epoch, severity, cancelled
        ? "Compression cancelled for " + target + "."
        : "Could not compress " + target + ": " + reason);
    if (cancelled) return {Kind::cancelled};
    return {Kind::failed, std::move(reason)};
}

nlohmann::json NotesService::compress_note(std::int64_t id, std::uint64_t epoch, bool skip_ineligible) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto source = impl_->compression_source_locked(id, skip_ineligible);
    if (!source) return {{"skipped", true}};
    const auto& note = *source;
    const auto workspace = impl_->store->snapshot();
    const auto& prompt = *workspace->find_prompt("note-compression");
    const auto definition = prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys,
        prompt, NoteRequestKind::source_step);
    const auto control = std::make_shared<Impl::CompressionControl>();
    control->name = note.name;
    impl_->compressions[id] = control;
    try {
        if (!impl_->background_jobs.launch(
            [owner = impl_.get(), control, definition, instruction = prompt.text, note, epoch](std::atomic_bool& cancel) {
                (void)owner->run_compression(control, definition, instruction, note, epoch, cancel, /*serialized=*/true);
            }, control->cancel)) throw std::runtime_error("Worker unavailable");
        impl_->store->append_event(EventSeverity::info, "Compression started for " + compression_target(id, note.name) + ".");
    } catch (...) {
        control->cancel->store(true);
        impl_->compressions.erase(id);
        throw ApplicationError(ErrorCode::command_queue_full, "Could not start compression. Try again.");
    }
    impl_->notify_compression(id, epoch);
    return {{"accepted", true}};
}

std::shared_ptr<OperationReply> NotesService::compress_summary_note(std::string connection_id,
    std::int64_t id, std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto source = impl_->compression_source_locked(id, /*skip_ineligible=*/true);
    if (!source) {
        reply->complete({{"compressed", false}});
        return reply;
    }
    const auto& note = *source;
    const auto workspace = impl_->store->snapshot();
    const auto& prompt = *workspace->find_prompt("note-compression");
    SharedCharacterDefinition definition;
    try {
        definition = prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys,
            prompt, NoteRequestKind::source_step);
    } catch (const ApplicationError& error) {
        // The summary line goes on to its next step, so Events keeps the reason.
        try {
            impl_->store->append_event(EventSeverity::error,
                "Could not compress " + compression_target(id, note.name) + ": " + error.what());
        } catch (...) { log_warn("Could not record compression outcome."); }
        throw;
    }
    const auto control = std::make_shared<Impl::CompressionControl>();
    control->name = note.name;
    // One flag: summary Cancel, connection close, and a vault change all stop this request.
    auto call = std::make_shared<Impl::SummaryCall>();
    const auto line = impl_->find_note_summary_line(connection_id, id);
    call->connection_id = std::move(connection_id);
    call->cancel = control->cancel;
    impl_->compressions[id] = control;
    impl_->summary_calls.push_back(call);
    try {
        if (!impl_->background_jobs.launch(
            [owner = impl_.get(), control, call, reply, definition, instruction = prompt.text, note, epoch, line](
                std::atomic_bool& cancel) {
                NoteSummaryCost cost;
                const auto result = owner->run_compression(
                    control, definition, instruction, note, epoch, cancel, /*serialized=*/false, line ? &cost : nullptr);
                {
                    const std::lock_guard lifecycle(owner->lifecycle_mutex);
                    if (line) line->cost.merge(cost);
                    owner->remove_summary_call(call);
                }
                switch (result.kind) {
                case Impl::CompressionResult::Kind::saved: reply->complete({{"compressed", true}}); return;
                case Impl::CompressionResult::Kind::discarded: reply->complete({{"compressed", false}}); return;
                case Impl::CompressionResult::Kind::cancelled:
                    if (owner->published_epoch.load() != epoch) reply->fail(ErrorCode::vault_changed, "The vault changed.");
                    else reply->fail(ErrorCode::operation_cancelled, std::string(summary_cancelled));
                    return;
                case Impl::CompressionResult::Kind::failed:
                    reply->fail(ErrorCode::internal_error, result.failure);
                    return;
                }
            }, control->cancel)) throw std::runtime_error("Worker unavailable");
        impl_->store->append_event(EventSeverity::info, "Compression started for " + compression_target(id, note.name) + ".");
    } catch (...) {
        control->cancel->store(true);
        impl_->compressions.erase(id);
        impl_->remove_summary_call(call);
        throw ApplicationError(ErrorCode::command_queue_full, "Could not start compression. Try again.");
    }
    impl_->notify_compression(id, epoch);
    return reply;
}

std::optional<std::string> flex_test_failure(const GenerationResult& result) {
    if (result.outcome != GenerationOutcome::completed)
        return "Flex test failed: " + (result.message.empty() ? std::string("The provider returned an error.") : result.message);
    if (!result.service_tier.empty() && result.service_tier != "flex")
        return "The provider did not use Flex for this request.";
    return std::nullopt;
}

std::shared_ptr<OperationReply> NotesService::test_flex(std::string provider_id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto workspace = impl_->store->snapshot();
    const auto* provider = workspace->find_provider(provider_id);
    if (!provider) throw ApplicationError(ErrorCode::not_found, "That provider was not found.");
    auto config = provider->config;
    bind_note_api_key(config, impl_->api_keys);
    if (!eligible_for_flex_processing(config))
        throw ApplicationError(ErrorCode::invalid_argument, "CHA does not use Flex with this provider.");
    config.flex_processing = true;
    config.stream = false;
    config.timeout_s = 900; // Flex can queue a request for minutes.
    config.web_search = WebSearchMode::off;
    // Chat Completions sends max_tokens, which the OpenAI Flex models refuse.
    if (config.api != ProviderApi::chat_completions) config.max_tokens = 1024;
    const auto definition = std::make_shared<const CharacterDefinition>(CharacterDefinition{
        .character = {"flex-test", "Flex Test"}, .provider = {provider->id, std::move(config)},
    });
    auto reply = std::make_shared<OperationReply>();
    if (!impl_->background_jobs.launch([owner = impl_.get(), reply, definition](std::atomic_bool& cancel) {
        try {
            ProviderClient client(definition, owner->openai_auth, owner->api_keys);
            const auto answer = collect_note_prompt(client, definition, "Reply with OK.", cancel);
            if (cancel.load() || answer.result.outcome == GenerationOutcome::cancelled)
                reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
            else if (const auto failure = flex_test_failure(answer.result))
                reply->fail(ErrorCode::invalid_argument, *failure);
            else reply->complete(nlohmann::json::object());
        } catch (const std::exception& error) {
            reply->fail(ErrorCode::invalid_argument, "Flex test failed: " + std::string(error.what()));
        } catch (...) {
            reply->fail(ErrorCode::internal_error, "Flex test failed. Try again.");
        }
    })) throw ApplicationError(ErrorCode::command_queue_full, "Could not start the Flex test. Try again.");
    return reply;
}

nlohmann::json NotesService::note_compression_status(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return compression_status_locked(id);
}

void NotesService::cancel_note_compression(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->cancel_note_compression_locked(id, epoch);
}

void NotesService::Impl::notify_compression(std::int64_t id, std::uint64_t epoch) noexcept {
    try { if (note_compression_changed) note_compression_changed(id, epoch); }
    catch (...) { log_warn("Could not deliver compression notification."); }
}

void NotesService::Impl::retire_compression(std::int64_t id,
    const std::shared_ptr<CompressionControl>& control, std::uint64_t epoch,
    EventSeverity severity, const std::string& message) noexcept {
    const auto found = compressions.find(id);
    if (found == compressions.end() || found->second != control) return;
    // Remove before event/notification delivery, even if delivery fails.
    compressions.erase(found);
    if (published_epoch.load() != epoch) return;
    try { if (!message.empty()) store->append_event(severity, message); }
    catch (...) { log_warn("Could not record compression outcome."); }
    notify_compression(id, epoch);
}

void NotesService::cancel_compressions_locked() {
    while (!impl_->compressions.empty()) {
        const auto [id, control] = *impl_->compressions.begin();
        control->cancel->store(true);
        impl_->retire_compression(id, control, impl_->published_epoch.load(), EventSeverity::info,
            "Compression cancelled for " + compression_target(id, control->name) + ".");
    }
}

nlohmann::json NotesService::compression_status_locked(std::int64_t id) {
    if (impl_->compressions.contains(id)) return {{"pending", true}};
    if (impl_->compressions.empty()) return {{"pending", false}};
    try {
        const auto items = impl_->store->list_note_items();
        const auto pending = [&](auto&& self, std::int64_t parent) -> bool {
            if (impl_->compressions.contains(parent)) return true;
            return std::any_of(items.begin(), items.end(), [&](const auto& item) {
                return item.parent_id == parent && self(self, item.id);
            });
        };
        return {{"pending", pending(pending, id)}};
    } catch (const NoteError& error) { rethrow_note(error); }
}

void NotesService::Impl::cancel_note_compression_locked(std::int64_t id, std::uint64_t epoch) {
    const auto found = compressions.find(id);
    if (found == compressions.end()) return;
    const auto control = found->second;
    control->cancel->store(true);
    retire_compression(id, control, epoch, EventSeverity::info,
        "Compression cancelled for " + compression_target(id, control->name) + ".");
}

std::vector<std::int64_t> NotesService::Impl::collect_compressions_locked(std::int64_t root) {
    std::vector<std::int64_t> result;
    if (!compressions.empty()) {
        const auto items = store->list_note_items();
        const auto collect = [&](auto&& self, std::int64_t parent) -> void {
            if (compressions.contains(parent)) result.push_back(parent);
            for (const auto& item : items)
                if (item.parent_id == parent) self(self, item.id);
        };
        collect(collect, root);
    }
    return result;
}

void NotesService::Impl::retire_deleted_compressions_locked(const std::vector<std::int64_t>& ids, std::uint64_t epoch) {
    for (const auto id : ids) {
        if (const auto found = compressions.find(id); found != compressions.end()) {
            const auto control = found->second;
            control->cancel->store(true);
            retire_compression(id, control, epoch, EventSeverity::info, "Compression cancelled: note deleted.");
        }
    }
}

nlohmann::json NotesService::summary_plan(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        const auto item = impl_->store->get_note_item(id);
        if (!item) throw ApplicationError(ErrorCode::not_found, std::string(missing_summary_target));
        const bool folder = item->kind == NoteItemKind::folder;
        require_summary_target(*item, folder);
        if (!folder && impl_->store->get_note_text(id).text.size() < note_compression_min_bytes)
            throw ApplicationError(ErrorCode::invalid_argument, "This note is under 4 KiB and does not need summaries.");
        const auto workspace = impl_->store->snapshot();
        (void)prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys,
            *workspace->find_prompt("summary-l1"));

        auto notes = nlohmann::json::array();
        const auto add_target = [&](const NoteItem& note) {
            notes.push_back({{"id", note.id}, {"name", note.name}, {"has_compressed", note.has_compressed},
                {"has_l1", note.has_l1}, {"has_l2", note.has_l2}});
        };
        if (!folder) {
            add_target(*item);
        } else {
            for (const auto& note : impl_->store->list_note_items()) {
                if (note.parent_id == id && note.kind == NoteItemKind::note && !note.is_summary && !note.summary_excluded
                    && impl_->store->get_note_text(note.id).text.size() >= note_compression_min_bytes)
                    add_target(note);
            }
        }
        return {{"notes", std::move(notes)}};
    } catch (const NoteError& error) { rethrow_note(error); }
}

std::shared_ptr<NoteSummaryLine> NotesService::Impl::find_note_summary_line(
    const std::string& connection_id, std::int64_t id) {
    const auto it = note_summary_lines.find({connection_id, id});
    return it == note_summary_lines.end() ? nullptr : it->second;
}

void NotesService::summary_line(std::string connection_id, std::int64_t id, bool finished, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto key = std::pair{std::move(connection_id), id};
    if (!finished) {
        const auto note = impl_->store->get_note_item(id);
        if (!note || note->kind != NoteItemKind::note) return;
        const auto workspace = impl_->store->snapshot();
        const auto* provider = workspace->find_provider(workspace->notes().compression_provider_id);
        auto line = std::make_shared<NoteSummaryLine>();
        if (provider) {
            const auto& config = provider->config;
            line->cost.openai = uses_openai_api_key(config);
            line->cost.gemini_flex = config.api == ProviderApi::google && eligible_for_flex_processing(config);
        }
        impl_->note_summary_lines.insert_or_assign(key, std::move(line));
        return;
    }

    const auto entry = impl_->note_summary_lines.extract(key);
    if (entry.empty()) return;
    const auto& line = *entry.mapped();
    const auto& cost = line.cost;
    if (!cost.openai && !cost.gemini_flex) return;
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - line.started_at;
    try {
        const auto note = impl_->store->get_note_item(id);
        const auto target = note_summary_target(id, note ? note->name : "", false);
        std::string amount = "unavailable";
        if (!cost.incomplete || cost.has_estimate) {
            std::ostringstream formatted;
            formatted.imbue(std::locale::classic());
            formatted << std::fixed << std::setprecision(10) << cost.usd;
            amount = formatted.str();
            amount.erase(amount.find_last_not_of('0') + 1);
            if (amount.ends_with('.')) amount.pop_back();
            amount = "$" + amount + " USD";
        }
        std::string providers;
        if (cost.openai)
            providers = cost.openai_flex && !cost.openai_other_or_unknown_tier ? "OpenAI Flex" : "OpenAI";
        if (cost.gemini_flex) {
            if (!providers.empty()) providers += " and ";
            providers += "Gemini Flex";
        }
        std::ostringstream event;
        event.imbue(std::locale::classic());
        event << "Summary line completed for " << target << ". Estimated " << providers
            << " token cost: " << amount << (cost.incomplete ? " (incomplete)." : ".")
            << " Elapsed time: " << std::fixed << std::setprecision(3) << elapsed.count() << " s.";
        impl_->store->append_event(EventSeverity::info, event.str());
    } catch (...) { log_warn("Could not record summary line cost."); }
}

std::shared_ptr<OperationReply> NotesService::generate_summary_part(std::string connection_id,
    std::int64_t id, SummaryPart part, std::string text, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto prompt = summary_prompt(part);
    const bool folder = folder_summary_part(part);
    auto target = note_summary_target(id, "", folder);
    SharedCharacterDefinition definition;
    std::string instruction;
    try {
        const auto item = impl_->store->get_note_item(id);
        if (!item) throw ApplicationError(ErrorCode::not_found, std::string(missing_summary_target));
        target = note_summary_target(id, item->name, folder);
        require_summary_target(*item, folder);
        if (folder) {
            if (text.size() > note_document_byte_limit)
                throw ApplicationError(ErrorCode::body_too_large, "The overview text exceeds 12 MiB.");
        } else {
            // Every step of a note reads its original text, so the steps share a prefix.
            text = impl_->store->get_note_text(id).text;
        }
        const auto workspace = impl_->store->snapshot();
        const auto& step_prompt = *workspace->find_prompt(prompt);
        definition = prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys,
            step_prompt, folder ? NoteRequestKind::flex : NoteRequestKind::source_step);
        if (!folder) instruction = step_prompt.text;
    } catch (const NoteError& error) {
        record_note_summary_failure(*impl_->store, part, target, error.what());
        rethrow_note(error);
    } catch (const ApplicationError& error) {
        record_note_summary_failure(*impl_->store, part, target, error.what());
        throw;
    } catch (...) {
        record_note_summary_failure(*impl_->store, part, target, summary_start_failed);
        throw;
    }

    auto reply = std::make_shared<OperationReply>();
    auto call = std::make_shared<Impl::SummaryCall>();
    const auto line = folder ? nullptr : impl_->find_note_summary_line(connection_id, id);
    call->connection_id = std::move(connection_id);
    impl_->summary_calls.push_back(call);
    try {
        if (impl_->background_jobs.launch(
            [owner = impl_.get(), call, reply, epoch, part, target, line,
             definition = std::move(definition), text = std::move(text),
             instruction = std::move(instruction)](std::atomic_bool& cancel) {
                NotePromptAnswer answer;
                NoteSummaryCost cost;
                ErrorCode code = ErrorCode::internal_error;
                std::string failure;
                try {
                    for (unsigned retries = 0;; ++retries) {
                        std::unique_ptr<ProviderClient> client;
                        {
                            const std::lock_guard lifecycle(owner->lifecycle_mutex);
                            if (cancel.load())
                                throw ApplicationError(ErrorCode::operation_cancelled, std::string(summary_cancelled));
                            owner->require_admitted(epoch);
                            client = std::make_unique<ProviderClient>(definition, owner->openai_auth, owner->api_keys,
                                owner->settings.test_hooks.note_summary_transport);
                        }
                        answer = collect_note_prompt(*client, definition, text, cancel, instruction,
                            line ? &cost : nullptr);
                        if (!answer.result.retryable || cancel.load() || retries == 3) break;
                        const auto seconds = 10 << retries;
                        if (owner->settings.test_hooks.note_compression_wait) owner->settings.test_hooks.note_compression_wait(seconds);
                        else for (int i = 0; i < seconds * 10 && !cancel.load(); ++i)
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    require_answer(answer, cancel, {
                        .refused = "The provider refused this request.",
                        .context_window = "Prompt exceeds the model's context window.",
                        .incomplete = "The provider returned an incomplete result.",
                        .no_text = "The provider returned no text.",
                        .too_large = "The result exceeds 12 MiB.",
                    });
                } catch (const ApplicationError& error) { code = error.code; failure = error.what(); }
                catch (...) { failure = "Check the Notes provider settings and try again."; }

                {
                    const std::lock_guard lifecycle(owner->lifecycle_mutex);
                    if (line) line->cost.merge(cost);
                    owner->remove_summary_call(call);
                    if (owner->published_epoch.load() != epoch) {
                        code = ErrorCode::vault_changed;
                        failure = "The vault changed.";
                    } else if (cancel.load() || code == ErrorCode::operation_cancelled) {
                        code = ErrorCode::operation_cancelled;
                        failure = summary_cancelled;
                        try {
                            owner->store->append_event(EventSeverity::info,
                                std::string(summary_part_label(part)) + " cancelled for " + target + ".");
                        } catch (...) { log_warn("Could not record note summary cancellation."); }
                    } else if (!failure.empty()) {
                        record_note_summary_failure(*owner->store, part, target, failure);
                    }
                }
                if (failure.empty()) reply->complete({{"markdown", std::move(answer.text)}});
                else reply->fail(code, failure);
            }, call->cancel)) return reply;
    } catch (...) {
        // Launch can throw if the worker thread cannot be created.
    }
    impl_->remove_summary_call(call);
    record_note_summary_failure(*impl_->store, part, target, summary_start_failed);
    throw ApplicationError(ErrorCode::command_queue_full, std::string(summary_start_failed));
}

void NotesService::commit_summary_part(std::int64_t id, SummaryPart part,
    const nlohmann::json& document, std::uint64_t epoch) {
    std::unique_lock lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (part != SummaryPart::l1 && part != SummaryPart::l2 && part != SummaryPart::l3)
        throw ApplicationError(ErrorCode::invalid_argument);
    const bool folder = folder_summary_part(part);
    auto target = note_summary_target(id, "", folder);
    std::int64_t document_id;
    try {
        if (const auto item = impl_->store->get_note_item(id)) target = note_summary_target(id, item->name, folder);
        if (!folder) {
            document_id = impl_->store->commit_note_level(id, part == SummaryPart::l1 ? 1 : 2, document,
                std::string(summary_part_label(part)) + " ready for " + target + ".");
            if (part == SummaryPart::l2) return;
        } else {
            document_id = impl_->store->commit_folder_summary(id, document).id;
        }
    } catch (const NoteError& error) {
        record_note_summary_failure(*impl_->store, part, target, error.what());
        rethrow_note(error);
    }
    try { impl_->interrupt_abandoned_audio(); }
    catch (...) { log_warn("Could not interrupt abandoned summary audio."); }
    try {
        const auto notify = impl_->note_audio_changed;
        lifecycle.unlock();
        if (notify) notify(document_id, epoch);
    } catch (...) { log_warn("Could not deliver summary audio notification."); }
}

void NotesService::abandon_summary_part(std::int64_t id, SummaryPart part, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (part != SummaryPart::l1 && part != SummaryPart::l2 && part != SummaryPart::l3)
        throw ApplicationError(ErrorCode::invalid_argument);
    try {
        const auto item = impl_->store->get_note_item(id);
        if (!item) throw ApplicationError(ErrorCode::not_found, std::string(missing_summary_target));
        record_note_summary_failure(*impl_->store, part, note_summary_target(id, item->name, folder_summary_part(part)),
            "the result could not be converted to a note");
    } catch (const NoteError& error) { rethrow_note(error); }
}

void NotesService::Impl::remove_summary_call(const std::shared_ptr<SummaryCall>& call) {
    std::erase(summary_calls, call);
}

void NotesService::Impl::cancel_summary_calls(std::optional<std::string_view> connection_id) {
    for (const auto& call : summary_calls)
        if (!connection_id || call->connection_id == *connection_id) call->cancel->store(true);
    std::erase_if(note_summary_lines, [&](const auto& line) {
        return !connection_id || line.first.first == *connection_id;
    });
}

void NotesService::cancel_summary_parts(std::string_view connection_id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->cancel_summary_calls(connection_id);
}

NoteItem NotesService::set_note_summary_excluded(std::int64_t id, bool excluded, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try { return impl_->store->set_note_summary_excluded(id, excluded); }
    catch (const NoteError& error) { rethrow_note(error); }
}

std::shared_ptr<OperationReply> NotesService::generate_session_summary(
    std::string connection_id, const FullSessionId& identity, std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (connection_id.empty()) throw ApplicationError(ErrorCode::invalid_argument);
    // Welcome uses a separate temporary database and has no Notes folder.
    const bool welcome = identity.forum_id == entrance_id && identity.session_id == welcome_id;
    const auto workspace = impl_->store->snapshot();
    if (welcome || !workspace->find_forum(identity.forum_id))
        throw ApplicationError(ErrorCode::invalid_argument, "Save summary is only available for configured forums.");
    if (impl_->summaries.contains(identity)) throw ApplicationError(ErrorCode::invalid_argument,
        "A summary is already being saved for this session.");
    std::string source, label;
    NoteSummaryTarget target;
    try {
        const auto sessions = impl_->sessions->list(identity.forum_id);
        const auto stored = std::ranges::find(sessions, identity, &StoredSession::identity);
        if (stored == sessions.end()) throw ApplicationError(ErrorCode::not_found, "The source session was not found.");
        label = stored->label;
        auto entries = impl_->sessions->history(identity);
        std::erase_if(entries, [](const TranscriptEntry& entry) {
            return trim_view(entry.text).empty() || (entry.kind != EntryKind::human
                && !(entry.kind == EntryKind::character && entry.status == EntryStatus::complete));
        });
        if (entries.empty()) throw ApplicationError(ErrorCode::invalid_argument, "This session has no saved messages to summarize.");
        source = session_markdown(label, entries);
        target = impl_->store->summary_target(identity);
    } catch (const NoteError& error) { rethrow_note(error); }
    catch (const SessionNotFoundError&) { throw ApplicationError(ErrorCode::not_found); }
    catch (const ForumNotFoundError&) { throw ApplicationError(ErrorCode::not_found); }
    const auto short_definition = prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys,
        *workspace->find_prompt("session-short-summary"));
    const auto long_definition = prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys,
        *workspace->find_prompt("session-long-summary"));
    if (impl_->next_summary_run > note_id_limit) throw ApplicationError(ErrorCode::internal_error);
    const auto run = std::make_shared<Impl::SummaryRun>();
    run->run_id = impl_->next_summary_run++;
    run->connection_id = std::move(connection_id);
    run->epoch = epoch;
    run->target = target;
    run->label = std::move(label);
    impl_->summaries.emplace(identity, run);
    try {
        impl_->store->append_event(EventSeverity::info, "Summary save started for " + summary_target_label(identity, run->label) + ".");
        if (!impl_->background_jobs.launch(
            [owner = impl_.get(), identity, run, short_definition, long_definition,
             reply, source = std::move(source)](std::atomic_bool& cancel) {
                nlohmann::json result;
                ErrorCode code = ErrorCode::internal_error;
                std::string failure;
                try {
                    const auto generate = [&](const SharedCharacterDefinition& definition) {
                        NotePromptAnswer answer;
                        for (unsigned retries = 0;; ++retries) {
                            std::unique_ptr<ProviderClient> client;
                            {
                                const std::lock_guard lifecycle(owner->lifecycle_mutex);
                                const auto found = owner->summaries.find(identity);
                                // A retired run still settles its reply, so the request does not stay outstanding.
                                if (found == owner->summaries.end() || found->second != run || cancel.load())
                                    throw ApplicationError(ErrorCode::operation_cancelled, "Summary generation was cancelled.");
                                owner->require_admitted(run->epoch);
                                client = std::make_unique<ProviderClient>(definition, owner->openai_auth, owner->api_keys);
                            }
                            answer = collect_note_prompt(*client, definition, source, cancel);
                            if (!answer.result.retryable || cancel.load() || retries == 3) break;
                            const auto seconds = 10 << retries;
                            if (owner->settings.test_hooks.note_compression_wait) owner->settings.test_hooks.note_compression_wait(seconds);
                            else for (int i = 0; i < seconds * 10 && !cancel.load(); ++i)
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        require_answer(answer, cancel, {
                            .refused = "The provider refused to summarize this session.",
                            .context_window = "The conversation exceeds the model's context window. "
                                "Select a model with a larger context window in Settings → Notes settings and try again.",
                            .incomplete = "The provider returned an incomplete summary.",
                            .no_text = "The provider returned no summary text.",
                            .too_large = "The generated summary exceeds 12 MiB.",
                        });
                        return std::move(answer.text);
                    };
                    auto short_text = generate(short_definition);
                    const auto long_text = generate(long_definition);
                    constexpr std::string_view short_heading = "# Short summary\n\n";
                    constexpr std::string_view long_heading = "\n\n# Long summary\n\n";
                    if (short_heading.size() + short_text.size() + long_heading.size() + long_text.size()
                        > note_document_byte_limit) {
                        throw ApplicationError(ErrorCode::body_too_large, "The generated summary exceeds 12 MiB.");
                    }
                    short_text.insert(0, short_heading);
                    short_text.append(long_heading).append(long_text);
                    {
                        const std::lock_guard lifecycle(owner->lifecycle_mutex);
                        const auto found = owner->summaries.find(identity);
                        if (found == owner->summaries.end() || found->second != run || cancel.load())
                            throw ApplicationError(ErrorCode::operation_cancelled, "Summary generation was cancelled.");
                        owner->require_admitted(run->epoch);
                        run->ready = true;
                        result = {{"run_id", run->run_id}, {"parent_id", run->target.parent_id},
                            {"target_note_id", run->target.target_note_id ? nlohmann::json(*run->target.target_note_id) : nlohmann::json(nullptr)},
                            {"markdown", std::move(short_text)}};
                    }
                } catch (const ApplicationError& error) { code = error.code; failure = error.what(); }
                catch (...) { failure = "Check the Notes provider settings and try again."; }
                // Delivery callbacks can acquire the router mutex. Never invoke them with the lifecycle lock held.
                if (failure.empty()) {
                    if (reply->complete(std::move(result))) return;
                    const std::lock_guard lifecycle(owner->lifecycle_mutex);
                    owner->retire_summary(identity, run, EventSeverity::info, "Summary save cancelled");
                } else {
                    {
                        const std::lock_guard lifecycle(owner->lifecycle_mutex);
                        if (code == ErrorCode::operation_cancelled)
                            owner->retire_summary(identity, run, EventSeverity::info, "Summary save cancelled");
                        else owner->retire_summary(identity, run, EventSeverity::error, "Summary save failed", failure);
                    }
                    reply->fail(code, failure);
                }
            }, run->cancel)) throw std::runtime_error("Worker unavailable");
    } catch (...) {
        impl_->retire_summary(identity, run, EventSeverity::error, "Summary save could not start");
        throw ApplicationError(ErrorCode::command_queue_full, "Could not start summary generation. Try again.");
    }
    return reply;
}

NoteItem NotesService::commit_session_summary(std::string_view connection_id, std::int64_t run_id,
    const nlohmann::json& document, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto found = std::find_if(impl_->summaries.begin(), impl_->summaries.end(),
        [run_id](const auto& entry) { return entry.second->run_id == run_id; });
    if (found == impl_->summaries.end()) throw ApplicationError(ErrorCode::not_found, "The summary run is no longer available.");
    const auto [identity, run] = *found;
    if (run->connection_id != connection_id || run->epoch != epoch)
        throw ApplicationError(ErrorCode::invalid_argument, "The summary run belongs to another window or vault.");
    try {
        if (run->cancel->load()) throw ApplicationError(ErrorCode::operation_cancelled, "Summary save was cancelled.");
        if (!run->ready) throw ApplicationError(ErrorCode::invalid_argument, "The summary is not ready to save.");
        if (!impl_->store->snapshot()->find_forum(identity.forum_id))
            throw ApplicationError(ErrorCode::not_found, "The source forum no longer exists.");
        auto item = impl_->store->commit_summary(identity, run->target, document);
        impl_->retire_summary(identity, run, EventSeverity::success, "");
        // The transaction has committed. Notification failures must not change the save result.
        try {
            if (!impl_->live_sessions->post_session_notice(epoch, identity, "“" + item.name + "” is stored in Notes."))
                log_warn("Could not deliver summary confirmation.");
        } catch (...) { log_warn("Could not deliver summary confirmation."); }
        try {
            impl_->interrupt_abandoned_audio();
            if (impl_->note_audio_changed) impl_->note_audio_changed(item.id, epoch);
        } catch (...) { log_warn("Could not deliver summary audio notification."); }
        return item;
    } catch (const NoteError& error) {
        impl_->retire_summary(identity, run, EventSeverity::error, "Summary save failed");
        rethrow_note(error);
    } catch (...) {
        impl_->retire_summary(identity, run, run->cancel->load() ? EventSeverity::info : EventSeverity::error,
            run->cancel->load() ? "Summary save cancelled" : "Summary save failed");
        throw;
    }
}

void NotesService::abandon_session_summary(std::string_view connection_id, std::int64_t run_id,
    std::string_view reason, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (reason != "conversion_failed" && reason != "commit_unknown") throw ApplicationError(ErrorCode::invalid_argument);
    const auto found = std::find_if(impl_->summaries.begin(), impl_->summaries.end(),
        [run_id](const auto& entry) { return entry.second->run_id == run_id; });
    if (found == impl_->summaries.end()) return;
    const auto [identity, run] = *found;
    if (run->connection_id != connection_id || run->epoch != epoch)
        throw ApplicationError(ErrorCode::invalid_argument, "The summary run belongs to another window or vault.");
    if (reason == "conversion_failed")
        impl_->retire_summary(identity, run, EventSeverity::error, "Summary save failed",
            "The generated summary could not be converted to a note.");
    else impl_->retire_summary(identity, run, EventSeverity::info, "Summary save abandoned after an unknown commit outcome");
}

std::shared_ptr<OperationReply> NotesService::read_note_page(
    std::string url, std::uint64_t epoch, std::function<void(std::string_view)> progress) {
    auto reply = std::make_shared<OperationReply>();
    WorkspaceWebSearch config;
    std::shared_ptr<const Workspace> workspace;
    std::string key;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        workspace = impl_->store->snapshot();
        config = workspace->web_search();
        if (config.read_provider == "firecrawl"
            && impl_->api_keys->find(config.firecrawl_api_key_id)) {
            key = impl_->api_keys->value(config.firecrawl_api_key_id);
        }
    }
    if (!config.web_reader_enabled && config.read_provider != "firecrawl") {
        throw ApplicationError(ErrorCode::invalid_argument,
            "Page reading is not configured. Enable web_reader or Firecrawl in Settings → Search API.");
    }
    if (!impl_->background_jobs.launch(
            [owner = impl_.get(), reply, workspace = std::move(workspace),
             config = std::move(config), key = std::move(key), url = std::move(url), epoch, progress = std::move(progress)](
                std::atomic_bool& cancel) {
                try {
                    // Document imports use the full article, rather than the model tool's 64 KiB limit.
                    const auto result = read_page(config, url, key, cancel,
                        firecrawl_scrape_endpoint, note_document_byte_limit, {}, true);
                    if (cancel.load()) {
                        reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
                        return;
                    }
                    auto page = nlohmann::json::parse(result);
                    if (page.at("truncated").get<bool>()) {
                        reply->fail(ErrorCode::body_too_large, "The web page is too large to import.");
                        return;
                    }
                    const bool youtube = is_youtube_url(url) || is_youtube_url(page.at("url").get<std::string>());
                    if (youtube || has_long_page_paragraph(page.at("markdown").get_ref<const std::string&>())) {
                        {
                            // The provider call uses vault credentials; do not start it after a vault change.
                            const std::lock_guard lifecycle(owner->lifecycle_mutex);
                            owner->require_admitted(epoch);
                        }
                        if (progress) progress(youtube ? "formatting_transcript" : "formatting_article");
                        auto title = page.at("title").get<std::string>();
                        page["markdown"] = format_note_page(*workspace,
                            page.at("markdown").get<std::string>(), title, youtube, owner->openai_auth,
                            owner->api_keys, cancel);
                        page["title"] = std::move(title);
                    }
                    reply->complete({{"url", page.at("url")}, {"title", page.at("title")},
                        {"markdown", page.at("markdown")}});
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (const WebToolError& error) {
                    reply->fail(ErrorCode::internal_error, error.what());
                } catch (const std::exception& error) {
                    log_warn("Note page import failed: " + std::string(error.what()));
                    reply->fail(ErrorCode::internal_error,
                        "Could not retrieve the web page. Try again.");
                }
            })) {
        reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

std::shared_ptr<OperationReply> NotesService::read_note_picture(
    std::string url, std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
    }
    if (!impl_->background_jobs.launch([reply, url = std::move(url)](std::atomic_bool& cancel) {
            try {
                const auto bytes = cha::read_note_picture(url, cancel);
                if (cancel.load()) {
                    reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
                    return;
                }
                reply->complete({{"base64", encode_base64(bytes)}});
            } catch (const WebToolError& error) {
                reply->fail(ErrorCode::internal_error, error.what());
            } catch (const std::exception& error) {
                log_warn("Note picture download failed: " + std::string(error.what()));
                reply->fail(ErrorCode::internal_error, "Could not load the picture.");
            }
        })) {
        reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

void NotesService::Impl::retire_summary(const FullSessionId& identity, const std::shared_ptr<SummaryRun>& run,
    EventSeverity severity, std::string_view message, std::string_view reason) noexcept {
    const auto found = summaries.find(identity);
    if (found == summaries.end() || found->second != run) return;
    run->cancel->store(true);
    summaries.erase(found);
    if (published_epoch.load() != run->epoch || message.empty()) return;
    try {
        store->append_event(severity, std::string(message) + " for " + summary_target_label(identity, run->label)
            + (reason.empty() ? "." : ": " + std::string(reason)));
    } catch (...) { log_warn("Could not record summary outcome."); }
}

void NotesService::cancel_summaries_locked(std::optional<std::string_view> connection_id) {
    impl_->cancel_summary_calls(connection_id);
    for (auto it = impl_->summaries.begin(); it != impl_->summaries.end();) {
        const auto [identity, run] = *it++;
        if (!connection_id || run->connection_id == *connection_id)
            impl_->retire_summary(identity, run, EventSeverity::info, "Summary save cancelled");
    }
}

void NotesService::cancel_session_summary_locked(const FullSessionId& identity) {
    const auto found = impl_->summaries.find(identity);
    if (found != impl_->summaries.end()) found->second->cancel->store(true);
}

} // namespace cha::app
