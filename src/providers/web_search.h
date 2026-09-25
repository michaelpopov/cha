#pragma once

#include "providers/provider_client.h"
#include "workspace/workspace.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace cha {

using WebSearchExecutor = std::function<std::string(
    const WorkspaceWebSearch&, std::string_view, const std::atomic_bool&)>;

// Returns source data for the model, with titles, URLs, and snippets.
std::string search_brave(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled,
    std::string_view endpoint = "https://api.search.brave.com/res/v1/web/search");
std::string search_tavily(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled,
    std::string_view endpoint = "https://api.tavily.com/search");

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
