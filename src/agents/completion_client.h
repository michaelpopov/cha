#pragma once

#include "agents/character.h"
#include "agents/completion_backend.h"

#include <atomic>

#include <memory>
#include <string>

namespace cha {

// The CompletionBackend for OpenAI-compatible HTTP endpoints, configured from
// one CharacterDefinition.
// It projects the transcript into provider messages, performs the request over libcurl as either a
// streaming or a single-response call, parses reasoning and answer content out of the provider
// format, and discovers a model when the configuration names none. It owns one connection handle,
// so a single client serves one request at a time.
class CompletionClient final : public CompletionBackend {
public:
    explicit CompletionClient(CharacterDefinition definition);
    ~CompletionClient() override;

    CompletionClient(const CompletionClient&) = delete;
    CompletionClient& operator=(const CompletionClient&) = delete;

    RequestPayload prepare(const CompletionInput& input) override;
    CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override;
    CompletionBackendInfo info() const override;

private:
    class CurlEasyHandle;

    void discover_model();
    std::string base_url() const;
    std::string endpoint() const;
    std::string models_endpoint() const;

    CharacterMetadata character_;
    CompletionConfig config_;
    std::string api_key_;
    std::unique_ptr<CurlEasyHandle> curl_;
    std::string system_prompt_;
};

} // namespace cha
