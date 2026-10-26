#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>
#include <nlohmann/json.hpp>

namespace cha {
inline constexpr int note_audio_poll_seconds = 10;
struct NoteAudioSizeError : std::runtime_error { using std::runtime_error::runtime_error; };
enum class NoteAudioFailure { transport, http, storage, invalid, uncertain, interrupted };
struct NoteAudioError : std::runtime_error {
    NoteAudioError(NoteAudioFailure kind, std::string message, long code = 0)
        : std::runtime_error(std::move(message)), kind(kind), code(code) {}
    NoteAudioFailure kind;
    long code;
};
struct NoteAudioService { std::string url, key, voice; };
using NoteAudioProgress = std::function<void(std::string, std::optional<std::size_t>, std::optional<std::size_t>)>;
// Validates settings and limits before the worker or any service request starts.
std::string prepare_note_audio(NoteAudioService& service, const std::vector<std::string>& blocks);
using NoteAudioCredentials = std::function<NoteAudioService()>;
nlohmann::json submit_note_audio(const NoteAudioCredentials& credentials, const std::string& jsonl,
    const std::atomic_bool& cancel);
std::string note_audio_batch_id(const nlohmann::json& operation);
nlohmann::json read_note_audio_batch(const NoteAudioCredentials& credentials, const std::string& batch_id,
    const std::atomic_bool& cancel);
void download_note_audio(const NoteAudioCredentials& credentials, const nlohmann::json& operation,
    std::size_t block_count, const std::filesystem::path& temporary, const std::atomic_bool& cancel);
std::vector<std::filesystem::path> process_note_audio(std::size_t block_count,
    const std::filesystem::path& temporary, const std::atomic_bool& cancel);
void wait_note_audio(int seconds, const std::atomic_bool& cancel, const std::function<void(int)>& wait = {});
// The caller owns the temporary directory and removes it after committing or failing.
std::vector<std::filesystem::path> generate_note_audio(
    const NoteAudioService& service, const std::string& jsonl, std::size_t block_count,
    const std::filesystem::path& temporary, const std::atomic_bool& cancel,
    const NoteAudioProgress& progress, const std::function<void(int)>& wait = {});
NoteAudioError note_audio_codec_error(std::string_view operation, long code);
void encode_note_audio(const std::filesystem::path& wav, const std::filesystem::path& m4a);
}
