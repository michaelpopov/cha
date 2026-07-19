#pragma once

#include "conversation.h"

#include <filesystem>
#include <vector>

namespace cha {

// This staged persistence boundary is not selected by the runtime yet. Each file is
// self-contained so a future compactor can publish a replacement generation.
[[nodiscard]] std::vector<ConversationMessage> load_conversation_file(const std::filesystem::path& path);
void save_conversation_file(const std::filesystem::path& path, const Conversation& conversation);

} // namespace cha
