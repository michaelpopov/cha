#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace cha::app {

enum class SummaryPart { l1, l2, l3_section, l3_overview, l3 };

using NoteAudioChanged = std::function<void(std::int64_t, std::uint64_t)>;

struct FolderStoreChoiceView {
    std::string service;
    std::string api_key_id;
    std::string api_key_name;
};
struct FolderStoreStart {
    std::int64_t operation_id{};
};
struct FolderStoreOperationStatus {
    bool present{};
    std::int64_t operation_id{};
    std::string phase;
    std::int64_t completed{};
    std::int64_t total{};
    std::string error;
    std::string result;
    bool known_cleanup_remains{};
    bool terminal{};
};

} // namespace cha::app
