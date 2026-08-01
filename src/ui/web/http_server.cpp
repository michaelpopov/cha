#include "ui/web/http_server.h"

#include "ui/web/http_response.h"
#include "ui/web/protocol.h"
#include "util/logging.h"

#include <httplib.h>

#include <exception>
#include <stdexcept>
#include <string>

namespace cha::web {
namespace {

void set_generated_error(httplib::Response& response) {
    if (!response.body.empty()) return;

    const int status = response.status;
    if (status == 404) {
        set_error_response(
            response,
            status,
            {ErrorCode::not_found, "The requested resource was not found."});
    } else if (status == 413) {
        set_error_response(
            response,
            status,
            {ErrorCode::body_too_large, "Request body is too large."});
    } else if (status < 500) {
        set_error_response(
            response,
            status,
            {ErrorCode::bad_request, "The HTTP request was not valid."});
    } else {
        set_error_response(
            response,
            status,
            {ErrorCode::internal_error, "The request could not be completed."});
    }
}

void set_exception_error(
    httplib::Response& response,
    const std::exception_ptr& exception) {
    try {
        if (exception) std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        log_error(error.what());
    } catch (...) {
        log_error("Unhandled non-standard exception in HTTP route");
    }
    set_error_response(
        response,
        500,
        {ErrorCode::internal_error, "The request could not be completed."});
}

} // namespace

void configure_http_server(httplib::Server& server, WebSettings settings) {
    const std::size_t minimum_workers =
        settings.session_limit + settings.http_request_headroom;
    if (settings.http_thread_pool_size < minimum_workers) {
        throw std::invalid_argument(
            "Web HTTP pool needs session-limit SSE workers plus request headroom");
    }
    if (settings.http_pending_request_limit < settings.http_thread_pool_size) {
        throw std::invalid_argument(
            "Web pending-request limit must cover the HTTP request pool");
    }
    server.new_task_queue = [settings] {
        return new httplib::ThreadPool(
            settings.http_thread_pool_size,
            settings.http_thread_pool_size,
            settings.http_pending_request_limit);
    };
    server.set_payload_max_length(settings.request_body_limit);
    server.set_read_timeout(settings.http_read_timeout);
    // cpp-httplib waits for writability before every content-provider write,
    // so this bounds lack of progress rather than total stream duration.
    server.set_write_timeout(settings.http_write_timeout);
    server.set_error_handler(
        [](const httplib::Request&, httplib::Response& response) {
            set_generated_error(response);
        });
    server.set_exception_handler(
        [](const httplib::Request&,
           httplib::Response& response,
           std::exception_ptr exception) {
            set_exception_error(response, exception);
        });
}

} // namespace cha::web
