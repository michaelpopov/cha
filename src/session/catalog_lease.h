#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>

namespace cha {

// Reports that a short-lived catalog mutation could not obtain its directory
// lease before its bounded deadline. Unlike SessionBusyError this is suitable
// for a caller to retry as a recoverable create failure.
class CatalogBusyError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Serializes creation in one sessions directory. It collaborates with
// SessionCatalog: callers hold it only while checking public names and
// publishing a newly leased database; readers never acquire it.
class CatalogLease {
public:
    using Now = std::function<std::chrono::steady_clock::time_point()>;
    using Backoff = std::function<void()>;

    CatalogLease() = delete;
    ~CatalogLease();
    CatalogLease(CatalogLease&&) noexcept;
    CatalogLease& operator=(CatalogLease&&) noexcept;
    CatalogLease(const CatalogLease&) = delete;
    CatalogLease& operator=(const CatalogLease&) = delete;

    [[nodiscard]] static CatalogLease acquire(
        const std::filesystem::path& sessions_directory,
        std::chrono::steady_clock::duration timeout = std::chrono::seconds(2),
        Now now = {},
        Backoff backoff = {});
    [[nodiscard]] static std::filesystem::path lock_path(
        const std::filesystem::path& sessions_directory);
    [[nodiscard]] bool active() const noexcept;

private:
    class Impl;
    explicit CatalogLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cha
