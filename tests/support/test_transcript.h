#pragma once

#include "chat/transcript.h"

#include <optional>
#include <string>
#include <utility>

namespace cha::test {

// Concise fixture builder for tests whose subject is not the factory call
// itself. Production uses HumanEntrySpec's named fields directly.
inline TranscriptEntry human_entry(
    EntryId id,
    EntryIdentity author,
    EntryIdentity addressed_to,
    std::string text,
    std::optional<RequestId> request_id = std::nullopt) {
    return make_human_entry({
        .id = id,
        .author = std::move(author),
        .addressed_to = std::move(addressed_to),
        .text = std::move(text),
        .request_id = request_id,
    });
}

} // namespace cha::test
