#pragma once

#include "workspace/workspace.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha {

// Only locally constructed diagnostics, safe to include in model tool results.
class WebToolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using WebSearchExecutor = std::function<std::string(
    const WorkspaceWebSearch&, std::string_view, const std::atomic_bool&)>;

// Returns provider JSON capped at 32 KiB, with explicit truncation when needed.
std::string search_brave(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled,
    std::string_view endpoint = "https://api.search.brave.com/res/v1/web/search");
std::string search_tavily(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled,
    std::string_view endpoint = "https://api.tavily.com/search");

using WebReadExecutor = std::function<std::string(
    const WorkspaceWebSearch&, std::string_view, const std::atomic_bool&, const std::function<void()>&)>;

inline constexpr std::string_view firecrawl_scrape_endpoint = "https://api.firecrawl.dev/v2/scrape";
inline constexpr std::size_t page_read_byte_limit = 64 * 1024;

// Returns title, URL, and Markdown as JSON. Model tools default to a 64 KiB cap.
std::string read_firecrawl(std::string_view url, std::string_view key,
    const std::atomic_bool& cancelled,
    std::string_view endpoint = firecrawl_scrape_endpoint,
    std::size_t output_byte_limit = 64 * 1024, bool include_images = false);

std::string read_web_reader(std::string_view url, std::string_view base_url,
    const std::atomic_bool& cancelled, std::size_t output_byte_limit = 64 * 1024, bool include_images = false);

// Enabled web_reader runs first; enabled Firecrawl is the fallback.
std::string read_page(const WorkspaceWebSearch& config, std::string_view url,
    std::string_view firecrawl_key, const std::atomic_bool& cancelled,
    std::string_view firecrawl_endpoint = firecrawl_scrape_endpoint,
    std::size_t output_byte_limit = page_read_byte_limit, const std::function<void()>& on_fallback = {},
    bool include_images = false);

// Downloads at most 1 MiB. Format validation uses the existing note picture checks.
std::string read_note_picture(std::string_view url, const std::atomic_bool& cancelled);

} // namespace cha
