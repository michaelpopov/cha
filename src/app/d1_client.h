#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha {

// Minimal Cloudflare D1 REST client for the sync queue tables. Network work
// stays outside SQLite transactions; the Worker in block 2 owns retries and
// scheduling. The default transport uses curl with the R2 timeouts
// (10-second connection timeout, 30-second low-speed timeout).
class D1Client {
public:
    struct Credentials {
        std::string account_id;
        std::string database_id;
        std::string api_token;
    };
    struct Row {
        std::int64_t rowid{};
        std::int64_t piece{};
        std::int64_t pieces{};
        std::string record;
    };
    struct VaultQueues {
        std::optional<std::string> local;
        std::vector<std::string> recipients;
    };
    enum class Failure {
        transport,
        http_status,
        protocol,
        missing_table,
        full_database,
        result_too_large,
        rate_limited,
        quota_exceeded,
        authentication,
    };
    struct Error {
        Failure failure{};
        std::string message;
    };
    // Performs POST url with the JSON body; returns {http_status, response}.
    using Transport = std::function<std::pair<long, std::string>(
        const std::string& url,
        const std::string& body,
        const std::vector<std::string>& headers)>;

    explicit D1Client(Credentials credentials, Transport transport = {},
        std::function<bool()> cancelled = {});
    // Test-only endpoint replacement so loopback servers can exercise the
    // real curl path. Production always uses the Cloudflare endpoint.
    void set_endpoint_for_tests(std::string url) { endpoint_override_ = std::move(url); }
    [[nodiscard]] const Credentials& credentials() const noexcept { return credentials_; }

    // Creates one queue table when absent.
    [[nodiscard]] std::optional<Error> create_queue_table(std::string_view table);
    // Names every table in the D1 database.
    [[nodiscard]] std::pair<std::vector<std::string>, std::optional<Error>> list_tables();
    // Finds the local queue and other machines' queues for this vault.
    [[nodiscard]] std::pair<VaultQueues, std::optional<Error>> list_vault_queues(
        std::string_view vault, std::string_view machine);
    // Reads up to limit rows in rowid order, starting after after_rowid.
    [[nodiscard]] std::pair<std::vector<Row>, std::optional<Error>> read_queue(
        std::string_view table, int limit, std::int64_t after_rowid = 0);
    // Sends one piece to every recipient with one INSERT per recipient.
    [[nodiscard]] std::optional<Error> insert_piece(
        const std::vector<std::string>& recipients,
        std::int64_t piece,
        std::int64_t pieces,
        std::string_view record_text);
    // Deletes the handled prefix of one queue.
    [[nodiscard]] std::optional<Error> delete_through(
        std::string_view table, std::int64_t last_rowid);
    [[nodiscard]] std::pair<std::int64_t, std::optional<Error>> count_rows(
        std::string_view table);
    [[nodiscard]] std::optional<Error> drop_table(std::string_view table);
    [[nodiscard]] std::string query_url() const;

    [[nodiscard]] static bool is_full_database_message(std::string_view message);
    [[nodiscard]] static bool is_result_too_large_message(std::string_view message);

private:
    Credentials credentials_;
    Transport transport_;
    std::optional<std::string> endpoint_override_;
};

} // namespace cha
