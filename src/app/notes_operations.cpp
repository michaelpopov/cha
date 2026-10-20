#include "app/application_internal.h"

#include "providers/web_search.h"
#include "storage/note_storage.h"
#include "util/base64.h"
#include "util/logging.h"

namespace cha::app {
namespace {

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
            std::string bytes;
            try {
                bytes = cha::read_note_picture(url, cancel);
            } catch (const std::exception&) {
                log_warn("Note picture could not be loaded; keeping the text");
            }
            if (cancel.load()) {
                reply->fail(ErrorCode::operation_cancelled, "The operation was cancelled.");
                return;
            }
            reply->complete({{"base64", encode_base64(bytes)}});
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
