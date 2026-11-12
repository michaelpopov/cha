#include "notes/notes_service_internal.h"

#include "providers/web_search.h"
#include "session/session_markdown.h"
#include "storage/not_found_error.h"
#include "storage/note_storage.h"
#include "util/base64.h"
#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"
#include "util/text_file.h"
#include "workspace/builtins.h"

#include <algorithm>
#include <initializer_list>
#include <thread>

namespace cha::app {
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

SharedCharacterDefinition prepare_note_provider(const Workspace& workspace, OpenAiOAuth* oauth, ApiKeyStore* api_keys,
    const WorkspacePrompt& prompt) {
    auto provider = note_editing_provider(workspace);
    provider.config.timeout_s = 0;
    // Bind legacy names to a stable reference, so deleting a key cannot make a
    // waiting job select a different key later created with that name.
    const bool uses_key = provider.config.mode == Mode::net && provider.config.auth == ProviderAuth::none;
    if (!uses_key) {
        if (!provider.config.api_key_id.empty() || !provider.config.api_key_env.empty())
            log_warn("Ignoring unused Notes provider API key settings.");
        provider.config.api_key_id.clear();
        provider.config.api_key_env.clear();
        if (provider.config.mode != Mode::net) provider.config.auth = ProviderAuth::none;
    }
    if (uses_key && provider.config.api_key_id.empty() && !provider.config.api_key_env.empty()) {
        const auto key = api_keys->find_by_name(provider.config.api_key_env);
        if (!key) throw ApplicationError(ErrorCode::invalid_argument, "Select an existing compression API key in Settings.");
        provider.config.api_key_id = key->id;
        provider.config.api_key_env.clear();
    }
    if (provider.config.mode == Mode::net && provider.config.auth == ProviderAuth::openai_subscription
        && oauth->status().state != OpenAiOAuthState::connected)
        throw ApplicationError(ErrorCode::invalid_argument, "Connect your OpenAI account in Settings before compressing notes.");
    const auto definition = std::make_shared<const CharacterDefinition>(CharacterDefinition{
        .character = {prompt.id, prompt.display_name}, .provider = std::move(provider),
        .system_prompt = prompt.text,
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

NotePromptAnswer collect_note_prompt(ProviderClient& client,
    const SharedCharacterDefinition& definition, std::string text, const std::atomic_bool& cancel) {
    GenerationRequest request{
        .history = std::make_shared<const ModelHistory>(),
        .run = {.target = definition->character, .author = {"", "User"}, .prompt_text = std::move(text)},
        .include_tool_instructions = false,
    };
    NotePromptAnswer answer;
    answer.result = client.perform(client.prepare(request), [&answer](GenerationDelta delta) {
        if (delta.kind == GenerationDeltaKind::answer) answer.text += delta.text;
    }, cancel);
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

nlohmann::json NotesService::compress_note(std::int64_t id, std::uint64_t epoch, bool skip_ineligible) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    NoteText note;
    try {
        const auto items = impl_->store->list_note_items();
        const auto item = std::find_if(items.begin(), items.end(), [id](const auto& item) { return item.id == id; });
        if (skip_ineligible && (item == items.end() || item->kind != NoteItemKind::note
            || item->is_summary || item->has_compressed || impl_->compressions.contains(id))) return {{"skipped", true}};
        if (item != items.end() && item->kind != NoteItemKind::note)
            throw NoteError(NoteFailure::invalid, "That item is not a note.");
        if (item != items.end() && item->is_summary)
            throw NoteError(NoteFailure::invalid, "Session summaries cannot be compressed.");
        if (impl_->compressions.contains(id))
            throw ApplicationError(ErrorCode::invalid_argument, "Compression is already running for this note.");
        note = impl_->store->get_note_text(id);
    } catch (const NoteError& error) { rethrow_note(error); }
    if (note.text.size() < note_compression_min_bytes) {
        if (skip_ineligible) return {{"skipped", true}};
        throw ApplicationError(ErrorCode::invalid_argument, "This note is under 4 KiB and does not need compression.");
    }
    if (note.text.find_first_not_of(" \t\r\n") == std::string::npos) {
        if (skip_ineligible) return {{"skipped", true}};
        throw ApplicationError(ErrorCode::invalid_argument, "This note has no text to compress.");
    }
    const auto workspace = impl_->store->snapshot();
    const auto definition = prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys,
        *workspace->find_prompt("note-compression"));
    const auto control = std::make_shared<Impl::CompressionControl>();
    control->name = note.name;
    impl_->compressions[id] = control;
    try {
        if (!impl_->background_jobs.launch([owner = impl_.get(), control, definition, note, epoch](std::atomic_bool& cancel) {
            const auto target = compression_target(note.id, control->name);
            const auto discarded = "Compression discarded for " + target
                + ": the saved text changed. Compress again to use the current text.";
            auto severity = EventSeverity::error;
            std::string outcome;
            try {
                std::unique_lock serial(owner->compression_mutex, std::defer_lock);
                while (!cancel.load() && !serial.try_lock_for(std::chrono::milliseconds(100))) {}
                NotePromptAnswer answer;
                for (unsigned retries = 0;; ++retries) {
                    std::unique_ptr<ProviderClient> client;
                    {
                        const std::lock_guard lifecycle(owner->lifecycle_mutex);
                        const auto found = owner->compressions.find(note.id);
                        if (found == owner->compressions.end() || found->second != control) return;
                        owner->require_admitted(epoch);
                        if (cancel.load()) throw ApplicationError(ErrorCode::operation_cancelled, "Cancelled.");
                        if (owner->store->get_note_text(note.id).text != note.text) {
                            owner->retire_compression(note.id, control, epoch, EventSeverity::warning, discarded);
                            return;
                        }
                        client = std::make_unique<ProviderClient>(definition, owner->openai_auth, owner->api_keys);
                    }
                    answer = collect_note_prompt(*client, definition, note.text, cancel);
                    if (!answer.result.retryable || cancel.load() || retries == 3) break;
                    const auto seconds = 10 << retries;
                    if (owner->settings.test_hooks.note_compression_wait) owner->settings.test_hooks.note_compression_wait(seconds);
                    else for (int i = 0; i < seconds * 10 && !cancel.load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                const auto& result = answer.result;
                const std::lock_guard lifecycle(owner->lifecycle_mutex);
                const auto found = owner->compressions.find(note.id);
                if (found == owner->compressions.end() || found->second != control) return;
                owner->require_admitted(epoch);
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
                if (owner->store->commit_compressed_note(note.id, note.text, answer.text,
                        "Compressed version ready for " + target + ".")) {
                    owner->retire_compression(note.id, control, epoch, EventSeverity::success, "");
                } else {
                    owner->retire_compression(note.id, control, epoch, EventSeverity::warning, discarded);
                }
                return;
            } catch (const ApplicationError& error) {
                if (cancel.load() || error.code == ErrorCode::operation_cancelled
                    || error.code == ErrorCode::server_stopping || error.code == ErrorCode::application_unavailable) {
                    severity = EventSeverity::info;
                    outcome = "Compression cancelled for " + target + ".";
                } else outcome = "Could not compress " + target + ": " + error.what();
            } catch (const NoteError& error) {
                outcome = "Could not compress " + target + ": " + error.what();
            } catch (const std::exception&) {
                outcome = "Could not compress " + target + ": Check the provider settings and try again.";
            } catch (...) { outcome = "Could not compress " + target + ": Try again."; }
            const std::lock_guard lifecycle(owner->lifecycle_mutex);
            owner->retire_compression(note.id, control, epoch, severity, outcome);
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
        if (!folder) {
            const auto bytes = impl_->store->get_note_text(id).text.size();
            if (bytes < note_compression_min_bytes)
                throw ApplicationError(ErrorCode::invalid_argument, "This note is under 4 KiB and does not need summaries.");
            if (bytes > text_file_byte_limit)
                throw ApplicationError(ErrorCode::invalid_argument,
                    "This note is over 256 KiB. Summaries are limited to 256 KiB of text.");
        }
        const auto workspace = impl_->store->snapshot();
        (void)prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys,
            *workspace->find_prompt("summary-l1"));

        auto notes = nlohmann::json::array();
        const auto add_target = [&](const NoteItem& note) {
            notes.push_back({{"id", note.id}, {"name", note.name}, {"has_l1", note.has_l1}, {"has_l2", note.has_l2}});
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

std::shared_ptr<OperationReply> NotesService::generate_summary_part(std::string connection_id,
    std::int64_t id, SummaryPart part, std::string text, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto prompt = summary_prompt(part);
    const bool folder = folder_summary_part(part);
    auto target = note_summary_target(id, "", folder);
    SharedCharacterDefinition definition;
    try {
        const auto item = impl_->store->get_note_item(id);
        if (!item) throw ApplicationError(ErrorCode::not_found, std::string(missing_summary_target));
        target = note_summary_target(id, item->name, folder);
        require_summary_target(*item, folder);
        if (folder) {
            if (text.size() > note_document_byte_limit)
                throw ApplicationError(ErrorCode::body_too_large, "The overview text exceeds 12 MiB.");
        } else {
            const bool section = part == SummaryPart::l3_section;
            text = impl_->store->get_note_text(id, section).text;
            if (text.size() > text_file_byte_limit) {
                std::string reason = "the note has more than 256 KiB of text";
                if (section && item->has_compressed) reason = "the compressed text has more than 256 KiB";
                else if (section) reason += ". Compress the note, then run Summarize again";
                throw ApplicationError(ErrorCode::invalid_argument, std::move(reason));
            }
        }
        const auto workspace = impl_->store->snapshot();
        definition = prepare_note_provider(*workspace, impl_->openai_auth, impl_->api_keys, *workspace->find_prompt(prompt));
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
    call->connection_id = std::move(connection_id);
    impl_->summary_calls.push_back(call);
    try {
        if (impl_->background_jobs.launch(
            [owner = impl_.get(), call, reply, epoch, part, target,
             definition = std::move(definition), text = std::move(text)](std::atomic_bool& cancel) {
                NotePromptAnswer answer;
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
                            client = std::make_unique<ProviderClient>(definition, owner->openai_auth, owner->api_keys);
                        }
                        answer = collect_note_prompt(*client, definition, text, cancel);
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
