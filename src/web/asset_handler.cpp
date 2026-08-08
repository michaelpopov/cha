#include "web/asset_handler.h"

#include "web/http_response.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "util/utf8_path.h"

#include <httplib.h>

#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cha::web {
namespace {

constexpr std::string_view shell_cache = "no-cache";
constexpr std::string_view asset_cache =
    "public, max-age=31536000, immutable";
constexpr std::string_view content_security_policy =
    "default-src 'none'; script-src 'self'; style-src 'self'; "
    "img-src 'self' data:; font-src 'self'; connect-src 'self'; "
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

void set_not_found(httplib::Response& response) {
    set_error_response(
        response,
        404,
        {ErrorCode::not_found, "The requested resource was not found."});
}

std::optional<std::string_view> content_type(
    const std::filesystem::path& path) {
    static const std::unordered_map<std::string, std::string_view> types{
        {".html", "text/html; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".woff2", "font/woff2"},
        {".json", "application/json"},
        {".map", "application/json"},
    };
    const auto found = types.find(path.extension().string());
    if (found == types.end()) return std::nullopt;
    return found->second;
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (!input.good() && !input.eof()) return std::nullopt;
    return contents;
}

// The shell answers both '/' and the session deep link, so the two routes
// share this rather than each setting the headers themselves.
void write_shell(httplib::Response& response, const std::string& shell) {
    response.set_header("Cache-Control", std::string(shell_cache));
    response.set_header(
        "Content-Security-Policy", std::string(content_security_policy));
    response.set_content(shell, "text/html; charset=utf-8");
}

bool is_below(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root) {
    auto candidate_part = candidate.begin();
    for (auto root_part = root.begin(); root_part != root.end();
         ++root_part, ++candidate_part) {
        if (candidate_part == candidate.end() || *candidate_part != *root_part) {
            return false;
        }
    }
    return candidate != root;
}

} // namespace

AssetHandler::AssetHandler(std::filesystem::path web_root)
    : web_root_(std::filesystem::weakly_canonical(std::move(web_root))) {
    const std::filesystem::path index = web_root_ / "index.html";
    if (!std::filesystem::is_regular_file(index)) {
        throw std::runtime_error(
            "Browser application is missing '" + utf8_path(index)
            + "'; run 'npm run stage' from webapp/.");
    }
    const std::optional<std::string> shell = read_file(index);
    if (!shell) {
        throw std::runtime_error(
            "Failed to read browser application shell '" + utf8_path(index) + "'.");
    }
    shell_ = *shell;
}

void AssetHandler::install(httplib::Server& server) const {
    const std::string shell = shell_;
    server.Get("/", [shell](const httplib::Request&, httplib::Response& response) {
        write_shell(response, shell);
    });
    const std::filesystem::path web_root = web_root_;
    server.Get(
        R"(/assets/([^/]+))",
        [web_root](
            const httplib::Request& request,
            httplib::Response& response) {
            const std::string filename = request.matches[1];
            if (!is_valid_route_component(filename)) {
                return set_not_found(response);
            }
            const std::filesystem::path candidate =
                std::filesystem::weakly_canonical(
                    web_root / "assets" / filename);
            const std::optional<std::string_view> type = content_type(candidate);
            if (!is_below(candidate, web_root) || !type
                || !std::filesystem::is_regular_file(candidate)) {
                return set_not_found(response);
            }
            const std::optional<std::string> contents = read_file(candidate);
            if (!contents) return set_not_found(response);
            response.set_header("Cache-Control", std::string(asset_cache));
            response.set_content(*contents, std::string(*type));
        });
}

void AssetHandler::set_shell(httplib::Response& response) const {
    write_shell(response, shell_);
}

} // namespace cha::web
