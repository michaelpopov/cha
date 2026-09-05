#include "web/r2_database_transfer.h"

#include "session/session_lease.h"
#include "session/workspace_session_database.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
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

constexpr std::string_view r2_url_variable = "CHA_R2_URL";
constexpr std::string_view r2_access_key_variable = "CHA_R2_ACCESS_KEY_ID";
constexpr std::string_view r2_secret_key_variable =
    "CHA_R2_SECRET_ACCESS_KEY";
constexpr std::array sidecar_suffixes{
    std::string_view("-journal"),
    std::string_view("-wal"),
    std::string_view("-shm"),
};

struct R2Settings {
    std::string url;
    std::string canonical_uri;
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
    return std::filesystem::absolute(path).lexically_normal();
}

std::string required_environment(std::string_view name) {
    const std::string variable(name);
    const char* const value = std::getenv(variable.c_str());
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(
            "Missing required environment variable '" + variable + "'");
    }
    return value;
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

R2Settings load_r2_settings(const std::filesystem::path& database) {
    const std::string raw_url = required_environment(r2_url_variable);
    const std::size_t scheme_end = raw_url.find("://");
    if (scheme_end == std::string::npos) {
        throw std::runtime_error(
            "Environment variable 'CHA_R2_URL' must be an absolute HTTPS URL");
    }
    const std::string_view scheme(raw_url.data(), scheme_end);
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t path_start = raw_url.find('/', authority_start);
    if ((scheme != "https" && scheme != "http")
        || path_start == std::string::npos || path_start == authority_start
        || raw_url.find_first_of("?#", path_start) != std::string::npos) {
        throw std::runtime_error(
            "Environment variable 'CHA_R2_URL' must be an absolute object URL "
            "without a query or fragment");
    }

    const std::string authority = raw_url.substr(
        authority_start, path_start - authority_start);
    if (authority.find('@') != std::string::npos
        || authority.find_first_of(" \t\r\n") != std::string::npos) {
        throw std::runtime_error(
            "Environment variable 'CHA_R2_URL' contains an invalid host");
    }
    if (scheme == "http" && !is_loopback_host(authority)) {
        throw std::runtime_error(
            "Environment variable 'CHA_R2_URL' must use HTTPS");
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
            "Environment variable 'CHA_R2_URL' must end with one bucket name");
    }
    const std::string canonical_uri = bucket_uri + "/"
        + encode_path_component(utf8_path(database.filename()));

    return {
        .url = std::string(scheme) + "://" + authority + canonical_uri,
        .canonical_uri = canonical_uri,
        .host = authority,
        .access_key = required_environment(r2_access_key_variable),
        .secret_key = required_environment(r2_secret_key_variable),
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

std::string sha256(std::string_view contents) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size{};
    if (EVP_Digest(
            contents.data(), contents.size(), digest.data(), &size,
            EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error("Failed to hash R2 request");
    }
    return hex_bytes(digest.data(), size);
}

std::string sha256_file(const std::filesystem::path& path) {
    using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
    DigestContext context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        throw std::runtime_error(
            "Failed to initialize database hash for '" + utf8_path(path) + "'");
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read database '" + utf8_path(path) + "'");
    }
    std::array<char, 64 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes = input.gcount();
        if (bytes > 0
            && EVP_DigestUpdate(
                   context.get(), buffer.data(), static_cast<std::size_t>(bytes))
                != 1) {
            throw std::runtime_error(
                "Failed to hash database '" + utf8_path(path) + "'");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "Failed while reading database '" + utf8_path(path) + "'");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size{};
    if (EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1) {
        throw std::runtime_error(
            "Failed to finish database hash for '" + utf8_path(path) + "'");
    }
    return hex_bytes(digest.data(), size);
}

std::string hmac_sha256(std::string_view key, std::string_view contents) {
    if (key.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("R2 signing key is too large");
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size{};
    if (HMAC(
            EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(contents.data()),
            contents.size(), digest.data(), &size) == nullptr) {
        throw std::runtime_error("Failed to sign R2 request");
    }
    return std::string(
        reinterpret_cast<const char*>(digest.data()), size);
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
        std::string(method) + "\n" + settings.canonical_uri + "\n\n"
        + canonical_headers + "\n\n" + std::string(signed_headers) + "\n"
        + std::string(payload_hash);
    const std::string scope = time.date + "/auto/s3/aws4_request";
    const std::string string_to_sign =
        std::string(algorithm) + "\n" + time.timestamp + "\n" + scope + "\n"
        + sha256(canonical_request);

    const std::string date_key = hmac_sha256(
        "AWS4" + settings.secret_key, time.date);
    const std::string region_key = hmac_sha256(date_key, "auto");
    const std::string service_key = hmac_sha256(region_key, "s3");
    const std::string signing_key = hmac_sha256(service_key, "aws4_request");
    const std::string signature = hmac_sha256(signing_key, string_to_sign);
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

void require_status(CurlHandle& curl, std::string_view operation) {
    long status{};
    require_curl(
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status),
        "Failed to read R2 response status");
    if (status < 200 || status >= 300) {
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

void publish_download(
    TemporaryPath& temporary,
    const std::filesystem::path& database) {
    std::filesystem::path backup = database;
    backup += ".bac";
    const bool database_exists = regular_file_if_present(database);
    const bool backup_exists = regular_file_if_present(backup);
    const std::filesystem::path old_backup = unique_sibling(database, "backup");
    bool backup_staged = false;
    bool database_staged = false;
    bool installed = false;

    try {
        if (database_exists && backup_exists) {
            rename_path(backup, old_backup, "Failed to stage previous backup");
            backup_staged = true;
        }
        if (database_exists) {
            rename_path(database, backup, "Failed to back up database");
            database_staged = true;
        }
        remove_sidecars(database);
        rename_path(
            temporary.get(), database, "Failed to install downloaded database");
        temporary.release();
        installed = true;
    } catch (...) {
        std::error_code ignored;
        if (!installed && database_staged) {
            std::filesystem::rename(backup, database, ignored);
        }
        if (!installed && backup_staged) {
            ignored.clear();
            std::filesystem::rename(old_backup, backup, ignored);
        }
        throw;
    }

    if (backup_staged) {
        std::error_code ignored;
        std::filesystem::remove(old_backup, ignored);
    }
    secure_workspace_session_database_files(database);
}

std::string busy_message(const std::filesystem::path& database) {
    return "Database already in use: '" + utf8_path(database) + "'";
}

} // namespace

R2DatabaseTransfer upload_database_to_r2(
    const std::filesystem::path& database_path,
    R2DatabaseLease lease_mode) {
    const std::filesystem::path database = normalize_database_path(database_path);
    std::optional<SessionLease> lease;
    if (lease_mode == R2DatabaseLease::acquire) {
        lease.emplace(SessionLease::acquire(database, busy_message(database)));
    }

    secure_workspace_session_database_files(database);
    checkpoint_workspace_session_database(database);
    if (inspect_workspace_session_database(database)
        != WorkspaceDatabaseState::valid_v2) {
        throw std::runtime_error(
            "Cannot upload invalid CHA database '" + utf8_path(database) + "'");
    }

    const R2Settings settings = load_r2_settings(database);
    const std::string payload_hash = sha256_file(database);
    const std::uintmax_t byte_count = std::filesystem::file_size(database);
    if (byte_count
        > static_cast<std::uintmax_t>(
            std::numeric_limits<curl_off_t>::max())) {
        throw std::runtime_error("Database is too large to upload to R2");
    }
    std::ifstream input(database, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read database '" + utf8_path(database) + "'");
    }

    (void)curl_global();
    CurlHandle curl;
    CurlHeaders headers;
    std::array<char, CURL_ERROR_SIZE> error{};
    configure_request(curl, headers, settings, "PUT", payload_hash, error);
    headers.append("Content-Type: application/vnd.sqlite3");
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
    return {.byte_count = byte_count};
}

R2DatabaseTransfer download_database_from_r2(
    const std::filesystem::path& database_path,
    R2DatabaseLease lease_mode) {
    const std::filesystem::path database = normalize_database_path(database_path);
    const std::filesystem::path parent = database.parent_path();
    if (!std::filesystem::is_directory(parent)) {
        throw std::runtime_error(
            "Database parent '" + utf8_path(parent) + "' does not exist");
    }
    std::optional<SessionLease> lease;
    if (lease_mode == R2DatabaseLease::acquire) {
        lease.emplace(SessionLease::acquire(database, busy_message(database)));
    }

    const R2Settings settings = load_r2_settings(database);
    TemporaryPath temporary(unique_sibling(database, "download"));
    create_private_file(temporary.get(), {});
    std::ofstream output(
        temporary.get(), std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error(
            "Failed to open temporary database '"
            + utf8_path(temporary.get()) + "'");
    }

    const std::string payload_hash = sha256({});
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
            "Failed to write downloaded database beside '"
            + utf8_path(database) + "'");
    }
    require_status(curl, "download");
    tighten_private_file(temporary.get());
    if (inspect_workspace_session_database(temporary.get())
        != WorkspaceDatabaseState::valid_v2) {
        throw std::runtime_error(
            "R2 download is not a valid CHA database");
    }

    const std::uintmax_t byte_count =
        std::filesystem::file_size(temporary.get());
    remove_sidecars(temporary.get());
    publish_download(temporary, database);
    return {.byte_count = byte_count};
}

} // namespace cha::web
