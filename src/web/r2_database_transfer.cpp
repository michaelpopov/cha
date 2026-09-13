#include "web/r2_database_transfer.h"

#include "session/session_lease.h"
#include "session/workspace_session_database.h"
#include "util/crypto.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "util/text.h"
#include "util/toml_file.h"
#include "web/application_config.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace cha::web {
namespace {

constexpr std::array sidecar_suffixes{
    std::string_view("-journal"),
    std::string_view("-wal"),
    std::string_view("-shm"),
};

struct R2Settings {
    std::string url;
    std::string canonical_uri;
    std::string canonical_query;
    std::string host;
    std::string access_key;
    std::string secret_key;
};

struct SigningTime {
    std::string timestamp;
    std::string date;
};

class CurlHandle {
public:
    CurlHandle()
        : handle_(curl_easy_init(), &curl_easy_cleanup) {
        if (!handle_) {
            throw std::runtime_error("Failed to create R2 HTTP handle");
        }
    }

    CURL* get() const noexcept { return handle_.get(); }

private:
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle_;
};

class CurlHeaders {
public:
    ~CurlHeaders() { curl_slist_free_all(headers_); }

    void append(const std::string& header) {
        curl_slist* const appended = curl_slist_append(headers_, header.c_str());
        if (appended == nullptr) {
            throw std::runtime_error("Failed to allocate R2 HTTP headers");
        }
        headers_ = appended;
    }

    curl_slist* get() const noexcept { return headers_; }

private:
    curl_slist* headers_{};
};

class R2ObjectNotFoundError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TemporaryPath {
public:
    explicit TemporaryPath(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~TemporaryPath() {
        if (!owned_) return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        for (const std::string_view suffix : sidecar_suffixes) {
            std::filesystem::path sidecar = path_;
            sidecar += suffix;
            std::filesystem::remove(sidecar, ignored);
        }
    }

    const std::filesystem::path& get() const noexcept { return path_; }
    void release() noexcept { owned_ = false; }

private:
    std::filesystem::path path_;
    bool owned_{true};
};

class CurlGlobal {
public:
    CurlGlobal() {
        const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error(
                "Failed to initialize R2 HTTP transport: "
                + std::string(curl_easy_strerror(result)));
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal& curl_global() {
    static CurlGlobal instance;
    return instance;
}

std::filesystem::path normalize_database_path(
    const std::filesystem::path& path) {
    if (path.empty() || path.filename().empty()) {
        throw std::invalid_argument("Database path must name a file");
    }
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

bool is_hex(char value) {
    return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

bool is_unreserved(unsigned char value) {
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9')
        || value == '-' || value == '_'
        || value == '.' || value == '~';
}

char upper_hex(char value) {
    return static_cast<char>(
        std::toupper(static_cast<unsigned char>(value)));
}

std::string canonicalize_path(std::string_view path) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(path.size());
    for (std::size_t index{}; index < path.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(path[index]);
        if (is_unreserved(value) || value == '/') {
            result.push_back(static_cast<char>(value));
        } else if (value == '%' && index + 2 < path.size()
                   && is_hex(path[index + 1]) && is_hex(path[index + 2])) {
            result.push_back('%');
            result.push_back(upper_hex(path[index + 1]));
            result.push_back(upper_hex(path[index + 2]));
            index += 2;
        } else {
            result.push_back('%');
            result.push_back(hex[value >> 4]);
            result.push_back(hex[value & 0x0f]);
        }
    }
    return result;
}

std::string encode_path_component(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(value.size());
    for (const unsigned char byte : value) {
        if (is_unreserved(byte)) {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(hex[byte >> 4]);
            result.push_back(hex[byte & 0x0f]);
        }
    }
    return result;
}

bool is_loopback_host(std::string_view authority) {
    const std::size_t port = authority.find(':');
    const std::string_view host = authority.substr(0, port);
    return host == "127.0.0.1" || host == "localhost";
}

R2Settings load_r2_settings(
    std::string_view object_name,
    const R2StorageKey& storage,
    std::string canonical_query = {}) {
    const std::string& raw_url = storage.url;
    const std::size_t scheme_end = raw_url.find("://");
    if (scheme_end == std::string::npos) {
        throw std::runtime_error(
            "R2 key field 'url' must be an absolute HTTPS URL");
    }
    const std::string_view scheme(raw_url.data(), scheme_end);
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t path_start = raw_url.find('/', authority_start);
    if ((scheme != "https" && scheme != "http")
        || path_start == std::string::npos || path_start == authority_start
        || raw_url.find_first_of("?#", path_start) != std::string::npos) {
        throw std::runtime_error(
            "R2 key field 'url' must be an absolute bucket URL "
            "without a query or fragment");
    }

    const std::string authority = raw_url.substr(
        authority_start, path_start - authority_start);
    if (authority.find('@') != std::string::npos
        || authority.find_first_of(" \t\r\n") != std::string::npos) {
        throw std::runtime_error(
            "R2 key field 'url' contains an invalid host");
    }
    if (scheme == "http" && !is_loopback_host(authority)) {
        throw std::runtime_error(
            "R2 key field 'url' must use HTTPS");
    }

    std::string_view bucket_path =
        std::string_view(raw_url).substr(path_start);
    if (bucket_path.size() > 1 && bucket_path.ends_with('/')) {
        bucket_path.remove_suffix(1);
    }
    const std::string bucket_uri = canonicalize_path(bucket_path);
    if (bucket_uri.front() != '/' || bucket_uri.size() == 1
        || bucket_uri.find('/', 1) != std::string::npos) {
        throw std::runtime_error(
            "R2 key field 'url' must end with one bucket name");
    }
    std::string canonical_uri = bucket_uri;
    if (!object_name.empty()) {
        canonical_uri += "/" + encode_path_component(object_name);
    }
    std::string url = std::string(scheme) + "://" + authority + canonical_uri;
    if (!canonical_query.empty()) url += "?" + canonical_query;

    return {
        .url = std::move(url),
        .canonical_uri = canonical_uri,
        .canonical_query = std::move(canonical_query),
        .host = authority,
        .access_key = storage.access_key_id,
        .secret_key = storage.secret_key,
    };
}

std::string hex_bytes(const unsigned char* bytes, std::size_t size) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (std::size_t index{}; index < size; ++index) {
        result.push_back(hex[bytes[index] >> 4]);
        result.push_back(hex[bytes[index] & 0x0f]);
    }
    return result;
}

std::string sha256_hex(std::string_view contents) {
    const Sha256Digest digest = sha256_digest(contents);
    return hex_bytes(digest.data(), digest.size());
}

std::string sha256_file_hex(const std::filesystem::path& path) {
    const Sha256Digest digest = sha256_file_digest(path);
    return hex_bytes(digest.data(), digest.size());
}

std::string hmac_sha256_bytes(
    std::string_view key,
    std::string_view contents) {
    const Sha256Digest digest = hmac_sha256_digest(key, contents);
    return std::string(
        reinterpret_cast<const char*>(digest.data()), digest.size());
}

SigningTime signing_time() {
    const std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    if (gmtime_s(&utc, &now) != 0) {
#else
    if (gmtime_r(&now, &utc) == nullptr) {
#endif
        throw std::runtime_error("Failed to construct R2 request timestamp");
    }
    std::array<char, 17> timestamp{};
    std::array<char, 9> date{};
    if (std::strftime(
            timestamp.data(), timestamp.size(), "%Y%m%dT%H%M%SZ", &utc) == 0
        || std::strftime(date.data(), date.size(), "%Y%m%d", &utc) == 0) {
        throw std::runtime_error("Failed to format R2 request timestamp");
    }
    return {timestamp.data(), date.data()};
}

std::string authorization_header(
    std::string_view method,
    const R2Settings& settings,
    std::string_view payload_hash,
    const SigningTime& time) {
    constexpr std::string_view algorithm = "AWS4-HMAC-SHA256";
    constexpr std::string_view signed_headers =
        "host;x-amz-content-sha256;x-amz-date";
    const std::string canonical_headers =
        "host:" + settings.host + "\n"
        + "x-amz-content-sha256:" + std::string(payload_hash) + "\n"
        + "x-amz-date:" + time.timestamp;
    const std::string canonical_request =
        std::string(method) + "\n" + settings.canonical_uri + "\n"
        + settings.canonical_query + "\n"
        + canonical_headers + "\n\n" + std::string(signed_headers) + "\n"
        + std::string(payload_hash);
    const std::string scope = time.date + "/auto/s3/aws4_request";
    const std::string string_to_sign =
        std::string(algorithm) + "\n" + time.timestamp + "\n" + scope + "\n"
        + sha256_hex(canonical_request);

    const std::string date_key = hmac_sha256_bytes(
        "AWS4" + settings.secret_key, time.date);
    const std::string region_key = hmac_sha256_bytes(date_key, "auto");
    const std::string service_key = hmac_sha256_bytes(region_key, "s3");
    const std::string signing_key = hmac_sha256_bytes(service_key, "aws4_request");
    const std::string signature = hmac_sha256_bytes(signing_key, string_to_sign);
    return "Authorization: " + std::string(algorithm) + " Credential="
        + settings.access_key + "/" + scope + ", SignedHeaders="
        + std::string(signed_headers) + ", Signature="
        + hex_bytes(
            reinterpret_cast<const unsigned char*>(signature.data()),
            signature.size());
}

void require_curl(CURLcode result, std::string_view operation) {
    if (result != CURLE_OK) {
        throw std::runtime_error(
            std::string(operation) + ": " + curl_easy_strerror(result));
    }
}

void configure_request(
    CurlHandle& curl,
    CurlHeaders& headers,
    const R2Settings& settings,
    std::string_view method,
    std::string_view payload_hash,
    std::array<char, CURL_ERROR_SIZE>& error) {
    const SigningTime time = signing_time();
    headers.append("Host: " + settings.host);
    headers.append("x-amz-content-sha256: " + std::string(payload_hash));
    headers.append("x-amz-date: " + time.timestamp);
    headers.append(authorization_header(
        method, settings, payload_hash, time));
    headers.append("Expect:");

    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_URL, settings.url.c_str()),
        "Failed to configure R2 URL");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()),
        "Failed to configure R2 headers");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error.data()),
        "Failed to configure R2 diagnostics");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L),
        "Failed to configure R2 connection timeout");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L),
        "Failed to configure R2 low-speed limit");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 30L),
        "Failed to configure R2 low-speed timeout");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L),
        "Failed to configure R2 transport");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L),
        "Failed to configure R2 redirects");
}

[[noreturn]] void fail_transfer(
    std::string_view operation,
    CURLcode result,
    const std::array<char, CURL_ERROR_SIZE>& error) {
    std::string detail = error.data();
    if (detail.empty()) detail = curl_easy_strerror(result);
    throw std::runtime_error(
        "R2 " + std::string(operation) + " failed: " + detail);
}

void require_status(
    CurlHandle& curl,
    std::string_view operation,
    std::string_view not_found_message = {}) {
    long status{};
    require_curl(
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status),
        "Failed to read R2 response status");
    if (status < 200 || status >= 300) {
        if (status == 404 && !not_found_message.empty()) {
            throw R2ObjectNotFoundError(std::string(not_found_message));
        }
        throw std::runtime_error(
            "R2 " + std::string(operation) + " failed with HTTP status "
            + std::to_string(status));
    }
}

std::size_t read_file(
    char* destination,
    std::size_t size,
    std::size_t count,
    void* context) {
    auto& input = *static_cast<std::ifstream*>(context);
    const std::size_t capacity = size * count;
    const std::size_t request = std::min(
        capacity,
        static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max()));
    input.read(destination, static_cast<std::streamsize>(request));
    if (input.gcount() == 0 && !input.eof()) return CURL_READFUNC_ABORT;
    return static_cast<std::size_t>(input.gcount());
}

std::size_t discard_response(
    char*, std::size_t size, std::size_t count, void*) {
    return size * count;
}

std::size_t write_file(
    char* source,
    std::size_t size,
    std::size_t count,
    void* context) {
    auto& output = *static_cast<std::ofstream*>(context);
    const std::size_t bytes = size * count;
    output.write(source, static_cast<std::streamsize>(bytes));
    return output ? bytes : 0;
}

std::size_t write_string(
    char* source,
    std::size_t size,
    std::size_t count,
    void* context) {
    auto& output = *static_cast<std::string*>(context);
    output.append(source, size * count);
    return size * count;
}

std::string xml_text(
    std::string_view xml,
    std::string_view element,
    std::size_t offset = 0) {
    const std::string open = "<" + std::string(element) + ">";
    const std::string close = "</" + std::string(element) + ">";
    const std::size_t begin = xml.find(open, offset);
    if (begin == std::string_view::npos) return {};
    const std::size_t content = begin + open.size();
    const std::size_t end = xml.find(close, content);
    if (end == std::string_view::npos) {
        throw std::runtime_error("R2 returned an invalid object listing");
    }
    return std::string(xml.substr(content, end - content));
}

std::string decode_xml_text(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index{}; index < value.size();) {
        if (value[index] != '&') {
            result.push_back(value[index++]);
            continue;
        }
        const std::size_t end = value.find(';', index + 1);
        if (end == std::string_view::npos) {
            throw std::runtime_error("R2 returned an invalid object listing");
        }
        const std::string_view entity = value.substr(index, end - index + 1);
        if (entity == "&amp;") result.push_back('&');
        else if (entity == "&lt;") result.push_back('<');
        else if (entity == "&gt;") result.push_back('>');
        else if (entity == "&quot;") result.push_back('"');
        else if (entity == "&apos;") result.push_back('\'');
        else throw std::runtime_error("R2 returned an invalid object listing");
        index = end + 1;
    }
    return result;
}

unsigned char hex_value(char value) {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned char>(value - 'A' + 10);
    }
    throw std::runtime_error("R2 returned an invalid encoded object name");
}

std::string decode_percent_encoding(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index{}; index < value.size(); ++index) {
        if (value[index] != '%') {
            result.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) {
            throw std::runtime_error("R2 returned an invalid encoded object name");
        }
        const unsigned char byte = static_cast<unsigned char>(
            (hex_value(value[index + 1]) << 4) | hex_value(value[index + 2]));
        result.push_back(static_cast<char>(byte));
        index += 2;
    }
    return result;
}

std::string list_objects(
    const R2StorageKey& storage,
    std::string_view continuation_token) {
    std::string query;
    if (!continuation_token.empty()) {
        query = "continuation-token="
            + encode_path_component(continuation_token) + "&";
    }
    query += "encoding-type=url&list-type=2";
    const R2Settings settings = load_r2_settings({}, storage, std::move(query));
    const std::string payload_hash = sha256_hex({});
    (void)curl_global();
    CurlHandle curl;
    CurlHeaders headers;
    std::array<char, CURL_ERROR_SIZE> error{};
    configure_request(curl, headers, settings, "GET", payload_hash, error);
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L),
        "Failed to configure R2 object listing");
    std::string response;
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_string),
        "Failed to configure R2 object listing writer");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response),
        "Failed to configure R2 object listing destination");
    const CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) fail_transfer("list", result, error);
    require_status(curl, "list");
    return response;
}

std::filesystem::path unique_sibling(
    const std::filesystem::path& database,
    std::string_view role) {
    static std::atomic_uint64_t serial{};
    for (std::size_t attempt{}; attempt != 100; ++attempt) {
        std::filesystem::path candidate = database;
        candidate += "." + std::string(role) + "."
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count())
            + "." + std::to_string(serial.fetch_add(1)) + ".tmp";
        std::error_code error;
        const bool exists = std::filesystem::exists(candidate, error);
        if (error) {
            throw std::filesystem::filesystem_error(
                "Failed to inspect temporary database path", candidate, error);
        }
        if (!exists) return candidate;
    }
    throw std::runtime_error(
        "Failed to choose a temporary path beside '" + utf8_path(database)
        + "'");
}

bool regular_file_if_present(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) return false;
    if (error) {
        throw std::filesystem::filesystem_error(
            "Failed to inspect database path", path, error);
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error(
            "Path '" + utf8_path(path) + "' is not a regular file");
    }
    return true;
}

void rename_path(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string_view operation) {
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            std::string(operation), source, destination, error);
    }
}

void remove_sidecars(const std::filesystem::path& database) {
    for (const std::string_view suffix : sidecar_suffixes) {
        std::filesystem::path sidecar = database;
        sidecar += suffix;
        std::error_code error;
        std::filesystem::remove(sidecar, error);
        if (error) {
            throw std::filesystem::filesystem_error(
                "Failed to remove stale database sidecar", sidecar, error);
        }
    }
}

void publish_downloads(
    TemporaryPath& database_temporary,
    TemporaryPath& vault_temporary,
    const std::filesystem::path& database,
    const std::filesystem::path& vault) {
    std::array targets{database, vault};
    std::array<TemporaryPath*, 2> temporaries{
        &database_temporary, &vault_temporary};
    std::array<std::filesystem::path, 2> backups;
    std::array<std::filesystem::path, 2> old_backups;
    std::array<bool, 2> target_exists{};
    std::array<bool, 2> backup_exists{};
    std::array<bool, 2> backup_staged{};
    std::array<bool, 2> target_staged{};
    std::array<bool, 2> installed{};

    for (std::size_t index{}; index < targets.size(); ++index) {
        backups[index] = targets[index];
        backups[index] += ".bac";
        target_exists[index] = regular_file_if_present(targets[index]);
        backup_exists[index] = regular_file_if_present(backups[index]);
        old_backups[index] = unique_sibling(targets[index], "backup");
    }

    try {
        for (std::size_t index{}; index < targets.size(); ++index) {
            if (target_exists[index] && backup_exists[index]) {
                rename_path(
                    backups[index], old_backups[index],
                    "Failed to stage previous backup");
                backup_staged[index] = true;
            }
        }
        for (std::size_t index{}; index < targets.size(); ++index) {
            if (!target_exists[index]) continue;
            rename_path(
                targets[index], backups[index], "Failed to back up local file");
            target_staged[index] = true;
        }
        remove_sidecars(database);
        for (std::size_t index{}; index < targets.size(); ++index) {
            rename_path(
                temporaries[index]->get(), targets[index],
                "Failed to install downloaded file");
            temporaries[index]->release();
            installed[index] = true;
        }
    } catch (...) {
        for (std::size_t index = targets.size(); index-- > 0;) {
            std::error_code ignored;
            if (installed[index]) {
                std::filesystem::remove(targets[index], ignored);
            }
            if (target_staged[index]) {
                ignored.clear();
                std::filesystem::rename(
                    backups[index], targets[index], ignored);
            }
            if (backup_staged[index]) {
                ignored.clear();
                std::filesystem::rename(
                    old_backups[index], backups[index], ignored);
            }
        }
        throw;
    }

    for (std::size_t index{}; index < targets.size(); ++index) {
        if (!backup_staged[index]) continue;
        std::error_code ignored;
        std::filesystem::remove(old_backups[index], ignored);
    }
    secure_workspace_session_database_files(database);
    tighten_private_file(vault);
}

std::uintmax_t upload_file(
    const std::filesystem::path& path,
    std::string_view object_name,
    std::string_view content_type,
    const R2StorageKey& storage) {
    const R2Settings settings = load_r2_settings(object_name, storage);
    const std::string payload_hash = sha256_file_hex(path);
    const std::uintmax_t byte_count = std::filesystem::file_size(path);
    if (byte_count
        > static_cast<std::uintmax_t>(
            std::numeric_limits<curl_off_t>::max())) {
        throw std::runtime_error("File is too large to upload to R2");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Failed to read '" + utf8_path(path) + "'");
    }

    (void)curl_global();
    CurlHandle curl;
    CurlHeaders headers;
    std::array<char, CURL_ERROR_SIZE> error{};
    configure_request(curl, headers, settings, "PUT", payload_hash, error);
    headers.append("Content-Type: " + std::string(content_type));
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L),
        "Failed to configure R2 upload");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, read_file),
        "Failed to configure R2 upload reader");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_READDATA, &input),
        "Failed to configure R2 upload source");
    require_curl(
        curl_easy_setopt(
            curl.get(), CURLOPT_INFILESIZE_LARGE,
            static_cast<curl_off_t>(byte_count)),
        "Failed to configure R2 upload size");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, discard_response),
        "Failed to configure R2 upload response");

    const CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) fail_transfer("upload", result, error);
    require_status(curl, "upload");
    return byte_count;
}

std::uintmax_t download_file(
    const std::filesystem::path& destination,
    std::string_view object_name,
    const R2StorageKey& storage,
    std::string_view not_found_message = {}) {
    const R2Settings settings = load_r2_settings(object_name, storage);
    create_private_file(destination, {});
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "Failed to open temporary download '" + utf8_path(destination) + "'");
    }

    const std::string payload_hash = sha256_hex({});
    (void)curl_global();
    CurlHandle curl;
    CurlHeaders headers;
    std::array<char, CURL_ERROR_SIZE> error{};
    configure_request(curl, headers, settings, "GET", payload_hash, error);
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_HTTPGET, 1L),
        "Failed to configure R2 download");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_file),
        "Failed to configure R2 download writer");
    require_curl(
        curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &output),
        "Failed to configure R2 download destination");

    const CURLcode result = curl_easy_perform(curl.get());
    output.close();
    if (result != CURLE_OK) fail_transfer("download", result, error);
    if (!output) {
        throw std::runtime_error(
            "Failed to write temporary download '"
            + utf8_path(destination) + "'");
    }
    require_status(curl, "download", not_found_message);
    tighten_private_file(destination);
    return std::filesystem::file_size(destination);
}

VaultDefinition require_matching_vault(
    const std::filesystem::path& database,
    const std::filesystem::path& vault) {
    if (!regular_file_if_present(vault)) {
        throw std::runtime_error(
            "Vault definition '" + utf8_path(vault) + "' does not exist");
    }
    const VaultDefinition definition =
        load_vault_definition_file(vault.parent_path(), vault);
    if (definition.data != database) {
        throw std::runtime_error(
            "Vault definition '" + utf8_path(vault)
            + "' does not name database '" + utf8_path(database) + "'");
    }
    return definition;
}

std::string busy_message(const std::filesystem::path& database) {
    return "Database already in use: '" + utf8_path(database) + "'";
}

} // namespace

R2DatabaseTransfer upload_database_to_r2(
    const std::filesystem::path& database_path,
    const std::filesystem::path& vault_definition_path,
    const R2StorageKey& storage,
    R2DatabaseLease lease_mode,
    std::string_view database_password) {
    const std::filesystem::path database = normalize_database_path(database_path);
    const std::filesystem::path vault =
        std::filesystem::absolute(vault_definition_path).lexically_normal();
    std::optional<SessionLease> lease;
    if (lease_mode == R2DatabaseLease::acquire) {
        lease.emplace(SessionLease::acquire(database, busy_message(database)));
    }

    secure_workspace_session_database_files(database);
    checkpoint_workspace_session_database(database, database_password);
    if (inspect_workspace_session_database(database, database_password)
        != WorkspaceDatabaseState::valid_v2) {
        throw std::runtime_error(
            "Cannot upload invalid CHA database '" + utf8_path(database) + "'");
    }
    (void)require_matching_vault(database, vault);

    const std::string database_name = utf8_path(database.filename());
    const std::uintmax_t vault_bytes = upload_file(
        vault, database_name + ".toml", "application/toml", storage);
    const std::uintmax_t database_bytes = upload_file(
        database, database_name, "application/vnd.sqlite3", storage);
    return {.byte_count = vault_bytes + database_bytes};
}

R2DatabaseTransfer download_database_from_r2(
    const std::filesystem::path& database_path,
    const std::filesystem::path& vault_definition_path,
    const R2StorageKey& storage,
    R2DatabaseLease lease_mode,
    std::string_view database_password) {
    const std::filesystem::path database = normalize_database_path(database_path);
    const std::filesystem::path vault =
        std::filesystem::absolute(vault_definition_path).lexically_normal();
    const std::filesystem::path parent = database.parent_path();
    if (!std::filesystem::is_directory(parent)) {
        throw std::runtime_error(
            "Database parent '" + utf8_path(parent) + "' does not exist");
    }
    std::optional<SessionLease> lease;
    if (lease_mode == R2DatabaseLease::acquire) {
        lease.emplace(SessionLease::acquire(database, busy_message(database)));
    }

    const VaultDefinition local = require_matching_vault(database, vault);
    const std::string database_name = utf8_path(database.filename());
    TemporaryPath database_temporary(unique_sibling(database, "download"));
    TemporaryPath vault_temporary(unique_sibling(vault, "download"));
    const std::string vault_object = database_name + ".toml";
    const std::uintmax_t vault_bytes = download_file(
        vault_temporary.get(), vault_object, storage,
        "R2 vault definition object '" + vault_object
            + "' was not found. The bucket may contain a legacy "
              "database-only upload; upload with the current CHA version "
              "before downloading.");
    const std::uintmax_t database_bytes = download_file(
        database_temporary.get(), database_name, storage);
    if (inspect_workspace_session_database(
            database_temporary.get(), database_password)
        != WorkspaceDatabaseState::valid_v2) {
        throw std::runtime_error(
            "R2 download is not a valid CHA database");
    }
    const VaultDefinition downloaded = load_vault_definition_file(
        vault.parent_path(), vault_temporary.get());
    if (!same_vault_name(downloaded.name, local.name)) {
        throw std::runtime_error(
            "R2 vault definition does not match the selected local vault");
    }
    rewrite_toml_file(vault_temporary.get(), [&](toml::table& table) {
        table.insert_or_assign("data", utf8_path(database));
    });

    remove_sidecars(database_temporary.get());
    publish_downloads(
        database_temporary, vault_temporary, database, vault);
    return {.byte_count = vault_bytes + database_bytes};
}

std::vector<std::string> list_r2_database_names(
    const R2StorageKey& storage) {
    constexpr std::string_view suffix = ".sqlite3";
    std::vector<std::string> names;
    std::string continuation_token;
    while (true) {
        const std::string response = list_objects(storage, continuation_token);
        std::size_t offset{};
        while (true) {
            const std::size_t key_start = response.find("<Key>", offset);
            if (key_start == std::string::npos) break;
            const std::size_t key_end = response.find("</Key>", key_start + 5);
            if (key_end == std::string::npos) {
                throw std::runtime_error("R2 returned an invalid object listing");
            }
            const std::string key = decode_percent_encoding(
                decode_xml_text(std::string_view(response).substr(
                    key_start + 5, key_end - key_start - 5)));
            if (key.size() > suffix.size() && key.ends_with(suffix)
                && key.find('/') == std::string::npos
                && key.find('\\') == std::string::npos) {
                names.push_back(key.substr(0, key.size() - suffix.size()));
            }
            offset = key_end + 6;
        }

        if (xml_text(response, "IsTruncated") != "true") break;
        const std::string next = decode_xml_text(
            xml_text(response, "NextContinuationToken"));
        if (next.empty() || next == continuation_token) {
            throw std::runtime_error("R2 returned an invalid object listing");
        }
        continuation_token = next;
    }
    std::sort(names.begin(), names.end(), [](const auto& left, const auto& right) {
        return fold_ascii(left) < fold_ascii(right);
    });
    return names;
}

R2DatabaseTransfer download_new_database_from_r2(
    const std::filesystem::path& database_path,
    const std::filesystem::path& vault_definition_path,
    std::string_view database_name,
    const R2StorageKey& storage) {
    constexpr std::string_view suffix = ".sqlite3";
    if (database_name.size() <= suffix.size()
        || !database_name.ends_with(suffix)
        || database_name.find('/') != std::string_view::npos
        || database_name.find('\\') != std::string_view::npos) {
        throw std::invalid_argument("Invalid R2 database name");
    }
    const std::filesystem::path database = normalize_database_path(database_path);
    const std::filesystem::path vault =
        std::filesystem::absolute(vault_definition_path).lexically_normal();
    if (!std::filesystem::is_directory(database.parent_path())
        || !std::filesystem::is_directory(vault.parent_path())) {
        throw std::invalid_argument("The database parent directory does not exist");
    }
    if (regular_file_if_present(database) || regular_file_if_present(vault)) {
        throw std::invalid_argument("The local vault already exists");
    }

    TemporaryPath database_temporary(unique_sibling(database, "download"));
    TemporaryPath vault_temporary(unique_sibling(vault, "download"));
    const std::string vault_object = std::string(database_name) + ".toml";
    std::uintmax_t vault_bytes{};
    bool downloaded_vault = true;
    try {
        vault_bytes = download_file(
            vault_temporary.get(), vault_object, storage,
            "R2 vault definition object '" + vault_object + "' was not found");
    } catch (const R2ObjectNotFoundError&) {
        downloaded_vault = false;
    }
    const std::uintmax_t database_bytes = download_file(
        database_temporary.get(), database_name, storage);
    std::optional<VaultDefinition> downloaded_definition;
    if (downloaded_vault) {
        downloaded_definition = load_vault_definition_file(
            vault.parent_path(), vault_temporary.get());
        rewrite_toml_file(vault_temporary.get(), [&](toml::table& table) {
            table.insert_or_assign("data", utf8_path(database));
        });
    } else {
        toml::table table;
        table.insert(
            "vault_name",
            std::string(database_name.substr(
                0, database_name.size() - suffix.size())));
        table.insert("data", utf8_path(database));
        write_toml_file(vault_temporary.get(), table);
    }
    if (downloaded_definition
        && downloaded_definition->password_protected) {
        throw std::runtime_error(
            "Protected vaults cannot be downloaded from R2 without a password");
    }
    if (inspect_workspace_session_database(database_temporary.get())
        != WorkspaceDatabaseState::valid_v2) {
        throw std::runtime_error("R2 download is not a valid CHA database");
    }
    remove_sidecars(database_temporary.get());
    publish_downloads(
        database_temporary, vault_temporary, database, vault);
    return {.byte_count = vault_bytes + database_bytes};
}

} // namespace cha::web
