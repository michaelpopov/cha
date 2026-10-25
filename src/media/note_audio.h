#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>

namespace cha {
inline constexpr int note_audio_poll_seconds = 10;
struct NoteAudioSizeError : std::runtime_error { using std::runtime_error::runtime_error; };
struct NoteAudioService { std::string url, key, voice; };
using NoteAudioProgress = std::function<void(std::string, std::optional<std::size_t>, std::optional<std::size_t>)>;
// Validates settings and limits before the worker or any service request starts.
std::string prepare_note_audio(NoteAudioService& service, const std::vector<std::string>& blocks);
// The caller owns the temporary directory and removes it after committing or failing.
std::vector<std::filesystem::path> generate_note_audio(
    const NoteAudioService& service, const std::string& jsonl, std::size_t block_count,
    const std::filesystem::path& temporary, const std::atomic_bool& cancel,
    const NoteAudioProgress& progress, const std::function<void(int)>& wait = {});
void encode_note_audio(const std::filesystem::path& wav, const std::filesystem::path& m4a);
}
