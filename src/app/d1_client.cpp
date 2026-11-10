#include "app/d1_client.h"

#include "storage/sync_storage.h"
#include "util/curl.h"

#include <curl/curl.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace cha {
namespace {

std::size_t append_bytes(char* data, std::size_t size, std::size_t count, void* target) {
    auto* text = static_cast<std::string*>(target);
    text->append(data, size * count);
    return size * count;
}

void require_curl(CURLcode result, std::string_view operation) {
    if (result != CURLE_OK) {
        throw std::runtime_error(
            std::string(operation) + ": " + curl_easy_strerror(result));
    }
}

std::pair<long, std::string> curl_post(
    const std::string& url,
    const std::string& body,
    const std::vector<std::string>& headers) {
    CurlHandle curl;
    CurlHeaders curl_headers;
    curl_headers.append("Content-Type: application/json");
    for (const auto& header : headers) curl_headers.append(header);
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()),
        "Failed to configure D1 URL");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, curl_headers.get()),
        "Failed to configure D1 headers");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L),
        "Failed to configure D1 connection timeout");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_LIMIT, 1L),
        "Failed to configure D1 low-speed limit");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_LOW_SPEED_TIME, 30L),
        "Failed to configure D1 low-speed timeout");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L),
        "Failed to configure D1 transport");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_POST, 1L),
        "Failed to configure D1 POST");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str()),
        "Failed to configure D1 body");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
        static_cast<curl_off_t>(body.size())),
        "Failed to configure D1 body size");
    std::string response;
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, append_bytes),
        "Failed to configure D1 writer");
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response),
        "Failed to configure D1 destination");
    require_curl(curl_easy_perform(curl.get()), "D1 request failed");
    long status = 0;
    require_curl(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status),
        "Failed to read D1 response status");
    return {status, response};
}

std::string endpoint(const D1Client::Credentials& credentials) {
    return "https://api.cloudflare.com/client/v4/accounts/" + credentials.account_id
        + "/d1/database/" + credentials.database_id + "/query";
}

} // namespace

D1Client::D1Client(Credentials credentials, Transport transport)
    : credentials_(std::move(credentials)), transport_(std::move(transport)) {
    if (!transport_) transport_ = curl_post;
}

std::string D1Client::query_url() const {
    if (endpoint_override_) return *endpoint_override_;
    return endpoint(credentials_);
}

namespace {

bool is_full_database_message(std::string_view message) {
    constexpr std::string_view prefix = "D1_ERROR: ";
    if (message.starts_with(prefix)) message.remove_prefix(prefix.size());
    return message == "Exceeded maximum DB size."
        || message == "Exceeded maximum DB size.: SQLITE_FULL";
}

bool is_result_too_large_message(std::string_view message) {
    return message.find("D1 query result exceeded") != std::string_view::npos;
}

using Json = nlohmann::json;
using Failure = D1Client::Failure;
using Error = D1Client::Error;

std::optional<Error> classify_error(const Json& error) {
    if (error.is_array()) {
        for (const auto& entry : error) {
            if (auto match = classify_error(entry)) return match;
        }
        return std::nullopt;
    }

    std::string_view message;
    if (error.is_string()) {
        message = error.get_ref<const std::string&>();
    } else if (error.is_object()) {
        const auto code = error.find("code");
        if (code != error.end() && (*code == 7403 || *code == 10000))
            return Error{Failure::authentication, "D1 rejected the API token."};
        const auto text = error.find("message");
        if (text != error.end() && text->is_string())
            message = text->get_ref<const std::string&>();
    }

    // Return fixed diagnostics because API errors can include request data.
    if (is_full_database_message(message))
        return Error{Failure::full_database, "Exceeded maximum DB size."};
    if (is_result_too_large_message(message))
        return Error{Failure::result_too_large, "D1 query result is too large."};
    if (message.find("no such table:") != std::string_view::npos)
        return Error{Failure::missing_table, "The D1 table does not exist."};
    if (message.find("D1's free tier daily row read limit") != std::string_view::npos)
        return Error{Failure::quota_exceeded, "D1 daily row read limit exceeded."};
    if (message.find("D1's free tier daily row write limit") != std::string_view::npos)
        return Error{Failure::quota_exceeded, "D1 daily row write limit exceeded."};
    return std::nullopt;
}

struct BatchResult {
    std::optional<Error> error;
    Json results;
};

BatchResult run_batch(const std::string& url,
    D1Client::Transport& transport,
    const D1Client::Credentials& credentials,
    const Json& batch) {
    const Json body = {{"batch", batch}};
    const std::vector<std::string> headers{
        "Authorization: Bearer " + credentials.api_token};
    std::pair<long, std::string> response;
    try {
        response = transport(url, body.dump(), headers);
    } catch (const Json::exception&) {
        return {.error = Error{Failure::protocol, "D1 request data is not valid JSON."}};
    } catch (const std::runtime_error& error) {
        return {.error = Error{Failure::transport, error.what()}};
    }
    const auto& [status, response_body] = response;
    if (status == 401 || status == 403)
        return {.error = Error{Failure::authentication, "D1 rejected the API token."}};

    auto parsed = Json::parse(response_body, nullptr, false);
    if (const auto errors = parsed.find("errors"); errors != parsed.end()) {
        if (auto error = classify_error(*errors)) return {.error = std::move(error)};
    }
    if (status == 429)
        return {.error = Error{Failure::rate_limited, "D1 request was rate limited (HTTP 429)."}};
    if (status < 200 || status >= 300)
        return {.error = Error{Failure::http_status,
            "D1 request failed with HTTP status " + std::to_string(status) + "."}};

    const auto success = parsed.find("success");
    const auto results = parsed.find("result");
    if (success == parsed.end() || *success != true
        || results == parsed.end() || !results->is_array()
        || results->size() != batch.size()) {
        return {.error = Error{Failure::protocol, "D1 did not confirm every statement in the batch."}};
    }

    for (const auto& entry : *results) {
        const auto ok = entry.find("success");
        if (ok == entry.end() || *ok != true) {
            const auto detail = entry.find("error");
            if (detail != entry.end()) {
                if (auto error = classify_error(*detail)) return {.error = std::move(error)};
            }
            return {.error = Error{Failure::protocol, "A D1 statement failed."}};
        }
    }
    return {.results = std::move(*results)};
}

nlohmann::json statement(std::string_view sql, const nlohmann::json& params) {
    return {{"sql", sql}, {"params", params}};
}

const Json& result_rows(const BatchResult& result) {
    const auto& rows = result.results.at(0).at("results");
    if (!rows.is_array()) throw std::runtime_error("D1 result rows are missing.");
    return rows;
}

std::int64_t integer_field(const Json& row, std::string_view name) {
    const auto& value = row.at(std::string(name));
    if (!value.is_number_integer()
        || (value.is_number_unsigned() && value.get<std::uint64_t>() > INT64_MAX))
        throw std::runtime_error("D1 result field is not a signed 64-bit integer.");
    return value.get<std::int64_t>();
}

} // namespace

std::optional<D1Client::Error> D1Client::create_queue_table(std::string_view table) {
    const std::string sql = "CREATE TABLE IF NOT EXISTS " + sync_escape_identifier(table)
        + " (piece INTEGER NOT NULL, pieces INTEGER NOT NULL, record TEXT NOT NULL)";
    BatchResult result = run_batch(query_url(), transport_, credentials_,
        nlohmann::json::array({statement(sql, nlohmann::json::array())}));
    return result.error;
}

std::pair<std::vector<std::string>, std::optional<D1Client::Error>> D1Client::list_tables() {
    BatchResult result = run_batch(query_url(), transport_, credentials_,
        nlohmann::json::array({statement(
            "SELECT name FROM sqlite_master WHERE type='table'", nlohmann::json::array())}));
    if (result.error) return {{}, result.error};
    std::vector<std::string> names;
    try {
        for (const auto& row : result_rows(result))
            names.push_back(row.at("name").get<std::string>());
    } catch (const std::exception&) {
        return {{}, Error{Failure::protocol, "Invalid D1 table list."}};
    }
    return {names, std::nullopt};
}

std::pair<D1Client::VaultQueues, std::optional<D1Client::Error>> D1Client::list_vault_queues(
    std::string_view vault, std::string_view machine) {
    auto [tables, error] = list_tables();
    if (error) return {{}, std::move(error)};
    VaultQueues queues;
    for (auto& table : tables) {
        const auto name = sync_parse_queue_table(table);
        if (!name || !sync_same_vault(name->vault, vault)) continue;
        if (sync_same_machine(name->machine, machine)) queues.local = std::move(table);
        else queues.recipients.push_back(std::move(table));
    }
    return {std::move(queues), std::nullopt};
}

std::pair<std::vector<D1Client::Row>, std::optional<D1Client::Error>> D1Client::read_queue(
    std::string_view table, int limit, std::int64_t after_rowid) {
    const std::string sql = "SELECT rowid, piece, pieces, record FROM "
        + sync_escape_identifier(table) + " WHERE rowid>?1 ORDER BY rowid LIMIT ?2";
    BatchResult result = run_batch(query_url(), transport_, credentials_,
        nlohmann::json::array({statement(sql,
            {std::to_string(after_rowid), std::to_string(std::max(1, limit))})}));
    if (result.error) return {{}, result.error};
    std::vector<Row> rows;
    try {
        for (const auto& row : result_rows(result)) {
            rows.push_back({integer_field(row, "rowid"), integer_field(row, "piece"),
                integer_field(row, "pieces"), row.at("record").get<std::string>()});
            // The query returns rows after after_rowid in increasing order.
            const std::int64_t previous =
                rows.size() > 1 ? rows[rows.size() - 2].rowid : after_rowid;
            if (rows.back().rowid <= previous)
                throw std::runtime_error("D1 queue rows are out of order.");
        }
    } catch (const std::exception&) {
        return {{}, Error{Failure::protocol, "Invalid D1 queue rows."}};
    }
    return {rows, std::nullopt};
}

std::optional<D1Client::Error> D1Client::insert_piece(
    const std::vector<std::string>& recipients,
    std::int64_t piece,
    std::int64_t pieces,
    std::string_view record_text) {
    if (recipients.empty()) return std::nullopt;
    nlohmann::json batch = nlohmann::json::array();
    for (const auto& table : recipients) {
        const std::string sql = "INSERT INTO " + sync_escape_identifier(table)
            + " (piece, pieces, record) VALUES (?1, ?2, ?3)";
        batch.push_back(statement(sql, {std::to_string(piece), std::to_string(pieces), record_text}));
    }
    BatchResult result = run_batch(query_url(), transport_, credentials_, batch);
    return result.error;
}

std::optional<D1Client::Error> D1Client::delete_through(
    std::string_view table, std::int64_t last_rowid) {
    const std::string sql =
        "DELETE FROM " + sync_escape_identifier(table) + " WHERE rowid<=?1";
    BatchResult result = run_batch(query_url(), transport_, credentials_,
        nlohmann::json::array({statement(sql, {std::to_string(last_rowid)})}));
    return result.error;
}

std::pair<std::vector<std::int64_t>, std::optional<D1Client::Error>> D1Client::count_rows(
    const std::vector<std::string>& tables) {
    if (tables.empty()) return {};
    Json batch = Json::array();
    for (const auto& table : tables) {
        const std::string sql =
            "SELECT COUNT(*) AS count FROM " + sync_escape_identifier(table);
        batch.push_back(statement(sql, Json::array()));
    }
    BatchResult result = run_batch(query_url(), transport_, credentials_, batch);
    if (result.error) return {{}, std::move(result.error)};
    std::vector<std::int64_t> counts;
    try {
        for (const auto& entry : result.results)
            counts.push_back(integer_field(entry.at("results").at(0), "count"));
    } catch (const std::exception&) {
        return {{}, Error{Failure::protocol, "Invalid D1 row count."}};
    }
    return {std::move(counts), std::nullopt};
}

std::optional<D1Client::Error> D1Client::drop_table(std::string_view table) {
    const std::string sql = "DROP TABLE " + sync_escape_identifier(table);
    BatchResult result = run_batch(query_url(), transport_, credentials_,
        nlohmann::json::array({statement(sql, nlohmann::json::array())}));
    return result.error;
}

} // namespace cha
