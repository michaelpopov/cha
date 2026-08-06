#include "ui/web/http_server.h"

#include "ui/web/http_response.h"
#include "ui/web/protocol.h"
#include "util/logging.h"

#include <httplib.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace cha::web {
namespace {

std::string lowercase_ascii(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

std::string unbracketed_host(std::string host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host.erase(host.begin());
        host.pop_back();
    }
    return lowercase_ascii(std::move(host));
}

std::string host_authority(std::string host, int port) {
    host = unbracketed_host(std::move(host));
    if (host.find(':') != std::string::npos) {
        host = '[' + host + ']';
    }
    return host + ':' + std::to_string(port);
}

std::unordered_set<std::string> allowed_host_authorities(
    std::string listener_host,
    int listener_port) {
    if (listener_host.empty()) {
        throw std::invalid_argument("Web listener host must not be empty");
    }
    if (listener_port < 1 || listener_port > 65535) {
        throw std::invalid_argument("Web listener port must be between 1 and 65535");
    }

    const std::string normalized = unbracketed_host(listener_host);
    std::unordered_set<std::string> allowed;
    const auto add = [&](const std::string& host) {
        const std::string authority = host_authority(host, listener_port);
        allowed.insert(authority);
        if (listener_port == 80) {
            allowed.insert(authority.substr(0, authority.size() - 3));
        }
    };
    add(normalized);
    if (normalized == "127.0.0.1"
        || normalized == "::1"
        || normalized == "localhost") {
        add("127.0.0.1");
        add("::1");
        add("localhost");
    }
    return allowed;
}

bool is_ip_literal(std::string_view host) {
    in_addr ipv4{};
    in6_addr ipv6{};
    const std::string value(host);
    return ::inet_pton(AF_INET, value.c_str(), &ipv4) == 1
        || ::inet_pton(AF_INET6, value.c_str(), &ipv6) == 1;
}

bool wildcard_authority_allowed(
    std::string authority,
    int listener_port) {
    authority = lowercase_ascii(std::move(authority));
    std::string host;
    std::string port;
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t closing = authority.find(']');
        if (closing == std::string::npos) return false;
        host = authority.substr(1, closing - 1);
        if (closing + 1 == authority.size() && listener_port == 80) {
            port = "80";
        } else if (closing + 1 < authority.size()
                   && authority[closing + 1] == ':') {
            port = authority.substr(closing + 2);
        } else {
            return false;
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            if (listener_port != 80) return false;
            host = std::move(authority);
            port = "80";
        } else {
            if (authority.find(':') != colon) return false;
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        }
    }
    return port == std::to_string(listener_port)
        && (host == "localhost" || is_ip_literal(host));
}

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
        // An unhandled route exception is the only record an operator gets of
        // why a request became a 500, so it keeps its message. Section 17
        // withholds prompt, answer, transcript, provider-message, and
        // credential text from logs; it does not withhold diagnostics.
        log_error(
            std::string("web server event=route_exception detail=")
            + error.what());
    } catch (...) {
        log_error("web server event=route_exception detail=non-standard exception");
    }
    set_error_response(
        response,
        500,
        {ErrorCode::internal_error, "The request could not be completed."});
}

} // namespace

void configure_http_server(
    httplib::Server& server,
    WebSettings settings,
    std::string listener_host,
    int listener_port) {
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
    const std::string normalized_listener = unbracketed_host(listener_host);
    const bool wildcard_listener = normalized_listener == "0.0.0.0"
        || normalized_listener == "::";
    const auto allowed_hosts = allowed_host_authorities(
        std::move(listener_host), listener_port);
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
    server.set_pre_routing_handler(
        [allowed_hosts, wildcard_listener, listener_port](
            const httplib::Request& request,
            httplib::Response& response) {
            const bool one_host = request.get_header_value_count("Host") == 1;
            const std::string authority = request.get_header_value("Host");
            const bool allowed = one_host
                && (allowed_hosts.contains(lowercase_ascii(authority))
                    || (wildcard_listener
                        && wildcard_authority_allowed(authority, listener_port)));
            if (allowed) return httplib::Server::HandlerResponse::Unhandled;
            set_error_response(
                response,
                403,
                {ErrorCode::forbidden_host, "Request host is not allowed."});
            return httplib::Server::HandlerResponse::Handled;
        });
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
