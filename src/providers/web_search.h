#pragma once

#include "providers/provider_client.h"
#include "workspace/workspace.h"

#include <atomic>
#include <functional>
#include <mutex>
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
    const WorkspaceWebSearch&, std::string_view, const std::atomic_bool&)>;

// Returns title, URL, and Markdown as JSON capped at 64 KiB.
std::string read_firecrawl(std::string_view url, std::string_view key,
    const std::atomic_bool& cancelled,
    std::string_view endpoint = "https://api.firecrawl.dev/v2/scrape");
std::string read_jina(std::string_view url, std::string_view key,
    const std::atomic_bool& cancelled,
    std::string_view endpoint = "https://r.jina.ai/");

// Shared by all recipients of one prompt. The first worker prepares the search;
// the other workers reuse its result before preparing their model requests.
struct WebSearchContext {
    WorkspaceWebSearch config;
    GenerationRequest query;
    SharedCharacterDefinition rewriter;

    const std::string& get(const ProviderClientFactory& factory,
        const WebSearchExecutor& search, const std::atomic_bool& cancelled);

private:
    std::once_flag once_;
    std::string context_;
};

} // namespace cha
