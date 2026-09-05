#pragma once

#include "characters/character.h"
#include "providers/model_backend.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cha {

class OpenAiOAuth;

// The narrow transport construction seam. Each call creates one independent
// backend from the same immutable character snapshot selected by the caller.
using ProviderClientFactory =
    std::function<std::unique_ptr<ModelBackend>(SharedCharacterDefinition)>;

// Test seam for one model HTTP POST. Production uses libcurl.
struct ProviderHttpRequest {
    std::string url;
    std::vector<std::string> headers;
    std::string body;
};

struct ProviderHttpResponse {
    long status = 0;
    std::string content_type;
    std::string body;
};

using ProviderHttpTransport = std::function<ProviderHttpResponse(
    const ProviderHttpRequest&,
    const std::atomic_bool& cancellation)>;

// The ModelBackend for OpenAI-compatible HTTP endpoints, configured from
// one CharacterDefinition.
// It projects the transcript into provider messages, performs the request over libcurl as either a
// streaming or a single-response call, parses reasoning and answer content out of the provider
// format. Each request-local client owns one connection handle.
class ProviderClient final : public ModelBackend {
public:
    explicit ProviderClient(SharedCharacterDefinition definition);
    ProviderClient(SharedCharacterDefinition definition, OpenAiOAuth* oauth);
    ProviderClient(
        SharedCharacterDefinition definition,
        OpenAiOAuth* oauth,
        ProviderHttpTransport transport);
    ~ProviderClient() override;

    ProviderClient(const ProviderClient&) = delete;
    ProviderClient& operator=(const ProviderClient&) = delete;

    RequestPayload prepare(const GenerationRequest& input) override;
    GenerationResult perform(
        RequestPayload payload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override;
private:
    class CurlEasyHandle;

    SharedCharacterDefinition definition_;
    OpenAiOAuth* oauth_{};
    ProviderHttpTransport transport_;
    std::string api_key_;
    std::unique_ptr<CurlEasyHandle> curl_;
};

} // namespace cha
