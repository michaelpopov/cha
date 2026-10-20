#include "app/application_internal.h"

#include "providers/web_search.h"
#include "storage/note_storage.h"
#include "util/base64.h"
#include "util/logging.h"

namespace cha {
std::string_view embedded_compression_prompt();
}

namespace cha::app {
namespace {

constexpr std::size_t note_compression_min_bytes = 4 * 1024;

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

std::shared_ptr<OperationReply> Application::compress_note(
    std::string_view connection_id, std::uint64_t request_id,
    std::int64_t id, std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    NoteText note;
    SharedCharacterDefinition definition;
    std::shared_ptr<PendingMediaRegistry::PendingMedia> pending;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        try { note = impl_->store->get_note_text(id); }
        catch (const NoteError& error) { rethrow_note(error); }
        if (note.text.size() < note_compression_min_bytes) {
            throw ApplicationError(ErrorCode::invalid_argument,
                "This note is under 4 KiB and does not need compression.");
        }
        if (note.text.find_first_not_of(" \t\r\n") == std::string::npos) {
            throw ApplicationError(ErrorCode::invalid_argument, "This note has no text to compress.");
        }
        const auto workspace = impl_->store->snapshot();
        const auto* provider = workspace->find_provider(workspace->notes().compression_provider_id);
        if (!provider) {
            throw ApplicationError(ErrorCode::invalid_argument,
                "Select a compression provider in Settings → Notes.");
        }
        const auto& effort = workspace->notes().compression_reasoning_effort;
        if (effort != "none"
            && !provider_supports_reasoning_effort(provider->config, provider->config.api)) {
            throw ApplicationError(ErrorCode::invalid_argument,
                "This compression provider's model does not support " + effort + " reasoning effort.");
        }
        auto config = provider->config;
        config.reasoning_effort = effort;
        config.timeout_s = 0;
        config.web_search = WebSearchMode::off;
        definition = std::make_shared<const CharacterDefinition>(CharacterDefinition{
            .character = {"note-compression", "Note compression"},
            .provider = {provider->id, std::move(config)},
            .system_prompt = std::string(embedded_compression_prompt()),
        });
        pending = impl_->pending_media.remember(std::string(connection_id), request_id);
    }
    try {
        if (impl_->background_jobs.launch(
                [owner = impl_.get(), reply, pending, definition = std::move(definition),
                 note = std::move(note), epoch](std::atomic_bool& cancel) {
                    PendingMediaRegistry::Cleanup cleanup{owner->pending_media, pending};
                    try {
                        GenerationRequest request{
                            .history = std::make_shared<const ModelHistory>(),
                            .run = {
                                .target = definition->character,
                                .author = {"", "User"},
                                .prompt_text = note.text,
                            },
                            .include_tool_instructions = false,
                        };
                        std::unique_ptr<ProviderClient> client;
                        try {
                            client = std::make_unique<ProviderClient>(
                                definition, owner->openai_auth.get(), owner->api_keys.get());
                        } catch (const std::exception& error) {
                            // Setup errors, such as a missing API key, are settings the user can fix.
                            throw ApplicationError(ErrorCode::invalid_argument, error.what());
                        }
                        std::string answer;
                        const auto result = client->perform(client->prepare(request),
                            [&answer](GenerationDelta delta) {
                                if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
                            }, *pending->cancelled);
                        {
                            // Cancel takes this lock too: either it prevents the save or the save wins.
                            const std::lock_guard lifecycle(owner->lifecycle_mutex);
                            if (pending->cancelled->load() || cancel.load()
                                || result.outcome == GenerationOutcome::cancelled) {
                                throw ApplicationError(ErrorCode::operation_cancelled,
                                    "The operation was cancelled.");
                            }
                            owner->require_admitted(epoch);
                            if (result.refused) {
                                throw ApplicationError(ErrorCode::internal_error,
                                    "The provider refused to compress this note.");
                            }
                            if (result.outcome != GenerationOutcome::completed) {
                                throw ApplicationError(ErrorCode::internal_error, result.message.empty()
                                    ? "The compression provider returned an error." : result.message);
                            }
                            if (result.finish_reason == "length" || result.finish_reason == "content_filter") {
                                throw ApplicationError(ErrorCode::internal_error,
                                    "The provider returned incomplete compressed text.");
                            }
                            owner->store->save_compressed_note(note.id, answer);
                        }
                        // A later Cancel must not turn a committed save into a cancellation reply.
                        reply->complete(nlohmann::json::object());
                    } catch (const NoteError& error) {
                        reply->fail(note_error_code(error.failure), error.what());
                    } catch (const ApplicationError& error) {
                        reply->fail(error.code, error.what());
                    } catch (const std::exception& error) {
                        log_warn("Note compression failed: " + std::string(error.what()));
                        reply->fail(ErrorCode::internal_error, "Try again.");
                    } catch (...) {
                        reply->fail(ErrorCode::internal_error, "Try again.");
                    }
                })) return reply;
    } catch (...) {
        impl_->pending_media.forget(pending);
        throw;
    }
    impl_->pending_media.forget(pending);
    reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
    return reply;
}

void Application::cancel_note_compression(
    std::string_view connection_id, std::uint64_t request_id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    impl_->pending_media.cancel(connection_id, request_id);
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
    std::string key;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        config = impl_->store->snapshot()->web_search();
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
            [reply, config = std::move(config), key = std::move(key), url = std::move(url)](
                std::atomic_bool& cancel) {
                try {
                    // Document imports use the full article, rather than the model tool's 64 KiB limit.
                    const auto result = read_page(config, url, key, cancel,
                        firecrawl_scrape_endpoint, note_document_byte_limit, {}, true);
                    if (cancel.load()) {
                        reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
                        return;
                    }
                    const auto page = nlohmann::json::parse(result);
                    if (page.at("truncated").get<bool>()) {
                        reply->fail(ErrorCode::body_too_large, "The web page is too large to import.");
                        return;
                    }
                    reply->complete({{"url", page.at("url")}, {"title", page.at("title")},
                        {"markdown", page.at("markdown")}});
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

void Application::save_note(
    std::int64_t id,
    const nlohmann::json& document,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        impl_->store->save_note(id, document);
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
        impl_->store->delete_note_item(id);
    } catch (const NoteError& error) {
        rethrow_note(error);
    }
}

} // namespace cha::app
