#include "app/application_internal.h"

#include "providers/web_search.h"
#include "storage/note_storage.h"
#include "util/base64.h"
#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"

#include <algorithm>
#include <initializer_list>
#include <thread>

namespace cha {
std::string_view embedded_compression_prompt();
std::string_view embedded_youtube_transcript_prompt();
std::string_view embedded_page_formatting_prompt();
}

namespace cha::app {
namespace {

constexpr std::size_t note_compression_min_bytes = 4 * 1024;

ProviderSelection note_editing_provider(const Workspace& workspace) {
    const auto* provider = workspace.find_provider(workspace.notes().compression_provider_id);
    if (!provider) {
        throw ApplicationError(ErrorCode::invalid_argument,
            "Select a compression provider in Settings → Notes.");
    }
    const auto& effort = workspace.notes().compression_reasoning_effort;
    if (effort != "none"
        && !provider_supports_reasoning_effort(provider->config, provider->config.api)) {
        throw ApplicationError(ErrorCode::invalid_argument,
            "This compression provider's model does not support " + effort + " reasoning effort.");
    }
    auto config = provider->config;
    config.reasoning_effort = effort;
    config.web_search = WebSearchMode::off;
    return {provider->id, std::move(config)};
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
    char* raw_host = nullptr;
    if (curl_url_get(parsed.get(), CURLUPART_HOST, &raw_host, 0) != CURLUE_OK) return false;
    const std::unique_ptr<char, decltype(&curl_free)> host(raw_host, curl_free);
    auto name = fold_ascii(host.get());
    if (name.ends_with('.')) name.pop_back();
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
    const auto definition = std::make_shared<const CharacterDefinition>(CharacterDefinition{
        .character = {youtube ? "youtube-transcript" : "page-formatting",
                      youtube ? "YouTube transcript" : "Page formatting"},
        .provider = note_editing_provider(workspace),
        .system_prompt = std::string(youtube ? embedded_youtube_transcript_prompt() : embedded_page_formatting_prompt()),
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

ErrorCode note_error_code(NoteFailure failure) {
    switch (failure) {
    case NoteFailure::invalid: return ErrorCode::invalid_argument;
    case NoteFailure::not_found: return ErrorCode::not_found;
    case NoteFailure::too_large: return ErrorCode::body_too_large;
    case NoteFailure::opening:
    case NoteFailure::saving:
        return ErrorCode::internal_error;
    }
    return ErrorCode::internal_error;
}

[[noreturn]] void rethrow_note(const NoteError& error) {
    throw ApplicationError(note_error_code(error.failure), error.what());
}

} // namespace

namespace {
std::string compression_target(std::int64_t id, std::string_view name) {
    return "“" + std::string(name) + "” (note " + std::to_string(id) + ")";
}
}

void Application::set_note_compression_changed(NoteAudioChanged callback) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->note_compression_changed = std::move(callback);
}

void Application::Impl::notify_compression(std::int64_t id, std::uint64_t epoch) noexcept {
    try { if (note_compression_changed) note_compression_changed(id, epoch); }
    catch (...) { log_warn("Could not deliver compression notification."); }
}

void Application::Impl::retire_compression(std::int64_t id,
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

void Application::Impl::cancel_compressions() {
    while (!compressions.empty()) {
        const auto [id, control] = *compressions.begin();
        control->cancel->store(true);
        retire_compression(id, control, published_epoch.load(), EventSeverity::info,
            "Compression cancelled for " + compression_target(id, control->name) + ".");
    }
}

nlohmann::json Application::compress_note(std::int64_t id, std::uint64_t epoch, bool skip_ineligible) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    NoteText note;
    try {
        const auto items = impl_->store->list_note_items();
        const auto item = std::find_if(items.begin(), items.end(), [id](const auto& item) { return item.id == id; });
        if (skip_ineligible && (item == items.end() || item->kind != NoteItemKind::note
            || item->has_compressed || impl_->compressions.contains(id))) return {{"skipped", true}};
        if (item != items.end() && item->kind != NoteItemKind::note)
            throw NoteError(NoteFailure::invalid, "That item is not a note.");
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
    auto provider = note_editing_provider(*impl_->store->snapshot());
    provider.config.timeout_s = 0;
    // Bind legacy names to a stable reference, so deleting a key cannot make a
    // waiting job select a different key later created with that name.
    if (provider.config.api_key_id.empty() && !provider.config.api_key_env.empty()) {
        const auto key = impl_->api_keys->find_by_name(provider.config.api_key_env);
        if (!key) throw ApplicationError(ErrorCode::invalid_argument, "Select an existing compression API key in Settings.");
        provider.config.api_key_id = key->id;
        provider.config.api_key_env.clear();
    }
    if (provider.config.mode == Mode::net && provider.config.auth == ProviderAuth::openai_subscription
        && impl_->openai_auth->status().state != OpenAiOAuthState::connected)
        throw ApplicationError(ErrorCode::invalid_argument, "Connect your OpenAI account in Settings before compressing notes.");
    const auto definition = std::make_shared<const CharacterDefinition>(CharacterDefinition{
        .character = {"note-compression", "Note compression"}, .provider = std::move(provider),
        .system_prompt = std::string(embedded_compression_prompt()),
    });
    // Validate before acceptance; the worker resolves the same key reference
    // again under the lifecycle lock before every actual request.
    try { ProviderClient validate(definition, impl_->openai_auth.get(), impl_->api_keys.get()); }
    catch (const std::exception& error) { throw ApplicationError(ErrorCode::invalid_argument, error.what()); }
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
                        client = std::make_unique<ProviderClient>(definition, owner->openai_auth.get(), owner->api_keys.get());
                    }
                    answer = collect_note_prompt(*client, definition, note.text, cancel);
                    if (!answer.result.retryable || cancel.load() || retries == 3) break;
                    const auto seconds = 10 << retries;
                    if (owner->settings.note_compression_wait) owner->settings.note_compression_wait(seconds);
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

nlohmann::json Application::note_compression_status(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return impl_->note_compression_status(id);
}

nlohmann::json Application::Impl::note_compression_status(std::int64_t id) {
    if (compressions.contains(id)) return {{"pending", true}};
    if (compressions.empty()) return {{"pending", false}};
    try {
        const auto items = store->list_note_items();
        const auto pending = [&](auto&& self, std::int64_t parent) -> bool {
            if (compressions.contains(parent)) return true;
            return std::any_of(items.begin(), items.end(), [&](const auto& item) {
                return item.parent_id == parent && self(self, item.id);
            });
        };
        return {{"pending", pending(pending, id)}};
    } catch (const NoteError& error) { rethrow_note(error); }
}

void Application::cancel_note_compression(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto found = impl_->compressions.find(id);
    if (found == impl_->compressions.end()) return;
    const auto control = found->second;
    control->cancel->store(true);
    impl_->retire_compression(id, control, epoch, EventSeverity::info,
        "Compression cancelled for " + compression_target(id, control->name) + ".");
}

std::vector<NoteItem> Application::list_note_items(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->list_note_items();
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

NoteItem Application::create_note_item(
    std::optional<std::int64_t> parent_id,
    NoteItemKind kind,
    std::string name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->create_note_item(parent_id, kind, name);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

std::int64_t Application::import_note_book(
    std::optional<std::int64_t> parent_id,
    std::string name, const std::vector<NoteImportEntry>& notes, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->import_note_book(parent_id, name, notes);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

NoteContent Application::get_note(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->get_note(id);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

std::shared_ptr<OperationReply> Application::read_note_page(
    std::string url, std::uint64_t epoch) {
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
            "Page reading is not configured. Enable web_reader or Firecrawl in Web search settings.");
    }
    if (!impl_->background_jobs.launch(
            [owner = impl_.get(), reply, workspace = std::move(workspace),
             config = std::move(config), key = std::move(key), url = std::move(url), epoch](
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
                        auto title = page.at("title").get<std::string>();
                        page["markdown"] = format_note_page(*workspace,
                            page.at("markdown").get<std::string>(), title, youtube, owner->openai_auth.get(),
                            owner->api_keys.get(), cancel);
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

std::shared_ptr<OperationReply> Application::read_note_picture(
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

bool Application::save_note(
    std::int64_t id,
    const nlohmann::json& document,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->save_note(id, document);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

void Application::save_note_scroll_position(
    std::int64_t id, std::int64_t scroll_top, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        impl_->store->save_note_scroll_position(id, scroll_top);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

NoteItem Application::set_note_read_only(std::int64_t id, bool read_only, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        auto item = impl_->store->set_note_read_only(id, read_only);
        if (!read_only) impl_->interrupt_abandoned_audio();
        return item;
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

NoteItem Application::rename_note_item(
    std::int64_t id,
    std::string name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->store->rename_note_item(id, name);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

void Application::delete_note_item(std::int64_t id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        // Capture affected controls before the delete. After it commits, retirement
        // must not depend on another fallible list read.
        std::vector<std::int64_t> deleted;
        if (!impl_->compressions.empty()) {
            const auto items = impl_->store->list_note_items();
            const auto collect = [&](auto&& self, std::int64_t parent) -> void {
                if (impl_->compressions.contains(parent)) deleted.push_back(parent);
                for (const auto& item : items)
                    if (item.parent_id == parent) self(self, item.id);
            };
            collect(collect, id);
        }
        impl_->store->delete_note_item(id);
        for (const auto note_id : deleted) {
            const auto control = impl_->compressions.at(note_id);
            control->cancel->store(true);
            impl_->retire_compression(note_id, control, epoch, EventSeverity::info,
                "Compression cancelled for " + compression_target(note_id, control->name) + ".");
        }
        impl_->interrupt_abandoned_audio();
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

} // namespace cha::app
