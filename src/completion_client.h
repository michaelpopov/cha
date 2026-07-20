#pragma once

#include "agent_definition.h"
#include "completion_backend.h"

#include <atomic>
#include <curl/curl.h>

#include <memory>
#include <string>

namespace cha {

// Performs one synchronous HTTP completion at a time and parses provider responses.
class CompletionClient final : public CompletionBackend {
public:
    explicit CompletionClient(AgentDefinition definition);

    CompletionClient(const CompletionClient&) = delete;
    CompletionClient& operator=(const CompletionClient&) = delete;

    [[nodiscard]] CompletionResult complete(
        const CompletionRequest& request,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override;
    [[nodiscard]] AgentInfo info() const override;
    [[nodiscard]] const std::string& agent_id() const override;

private:
    void discover_model();
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::string endpoint() const;
    [[nodiscard]] std::string models_endpoint() const;

    Config config_;
    std::string api_key_;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl_{nullptr, &curl_easy_cleanup};
    std::string system_prompt_;
};

} // namespace cha
