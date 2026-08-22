#pragma once

#include "agents/character.h"
#include "agents/model_backend.h"

#include <atomic>

#include <functional>
#include <memory>
#include <string>

namespace cha {

// The narrow transport construction seam. Each call creates one independent
// backend from the same immutable character snapshot selected by the caller.
using ProviderClientFactory =
    std::function<std::unique_ptr<ModelBackend>(SharedCharacterDefinition)>;

// Safe public runtime information comes directly from immutable configuration;
// neither helper resolves credentials nor constructs a transport.
ModelBackendInfo model_backend_info(const CharacterDefinition& definition);
std::string provider_endpoint(const ModelBackendConfig& config);

// The ModelBackend for OpenAI-compatible HTTP endpoints, configured from
// one CharacterDefinition.
// It projects the transcript into provider messages, performs the request over libcurl as either a
// streaming or a single-response call, parses reasoning and answer content out of the provider
// format. It owns one connection handle,
// so a single client serves one request at a time.
class ProviderClient final : public ModelBackend {
public:
    explicit ProviderClient(SharedCharacterDefinition definition);
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

    SharedCharacterDefinition definition_;
    std::string api_key_;
    std::unique_ptr<CurlEasyHandle> curl_;
};

} // namespace cha
