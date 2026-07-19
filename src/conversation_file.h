#pragma once

#include "conversation.h"

#include <filesystem>

namespace cha {

// Each file is self-contained, allowing a future compactor to publish a replacement generation.
[[nodiscard]] Conversation load_conversation_file(const std::filesystem::path& path);
void save_conversation_file(const std::filesystem::path& path, const Conversation& conversation);

} // namespace cha
