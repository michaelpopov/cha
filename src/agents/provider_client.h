#pragma once

#include "agents/character.h"
#include "agents/model_backend.h"

#include <atomic>

#include <memory>
#include <string>

namespace cha {

// The ModelBackend for OpenAI-compatible HTTP endpoints, configured from
// one CharacterDefinition.
// It projects the transcript into provider messages, performs the request over libcurl as either a
// streaming or a single-response call, parses reasoning and answer content out of the provider
// format. It owns one connection handle,
// so a single client serves one request at a time.
class ProviderClient final : public ModelBackend {
public:
    explicit ProviderClient(CharacterDefinition definition);
    ~ProviderClient() override;

    ProviderClient(const ProviderClient&) = delete;
    ProviderClient& operator=(const ProviderClient&) = delete;

    RequestPayload prepare(const GenerationRequest& input) override;
    GenerationResult perform(
        RequestPayload payload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override;
    ModelBackendInfo info() const override;

private:
    class CurlEasyHandle;

    std::string base_url() const;
    std::string endpoint() const;

    CharacterMetadata character_;
    ModelBackendConfig config_;
    std::string api_key_;
    std::unique_ptr<CurlEasyHandle> curl_;
    std::string system_prompt_;
};

} // namespace cha
