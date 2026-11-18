#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
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
// True for Google's own Gemini API origin, as prepare_note_audio() saves it.
bool google_note_audio_service(std::string_view origin);
// Validates settings and limits before the worker or any service request starts.
std::string prepare_note_audio(NoteAudioService& service, const std::vector<std::string>& blocks);
std::string synthesize_note_audio_preview(NoteAudioService service, std::string_view text, const std::atomic_bool& cancel);
using NoteAudioCredentials = std::function<NoteAudioService()>;
nlohmann::json list_note_audio_voices(const NoteAudioCredentials& credentials, const std::atomic_bool& cancel);
std::string upload_note_audio(const NoteAudioCredentials& credentials, const std::string& jsonl,
    const std::atomic_bool& cancel);
// A lost creation reply is uncertain: retrying it could create a second batch.
nlohmann::json create_note_audio_batch(const NoteAudioCredentials& credentials, const std::string& input,
    const std::atomic_bool& cancel);
std::string note_audio_batch_id(const nlohmann::json& operation);
nlohmann::json read_note_audio_batch(const NoteAudioCredentials& credentials, const std::string& batch_id,
    const std::atomic_bool& cancel);
void download_note_audio(const NoteAudioCredentials& credentials, const nlohmann::json& operation,
    std::size_t block_count, const std::filesystem::path& temporary, const std::atomic_bool& cancel);
struct NoteAudioUsage {
    std::uint64_t input_tokens{}, output_tokens{};
    std::size_t reported_requests{};
};
std::vector<std::filesystem::path> process_note_audio(std::size_t block_count,
    const std::filesystem::path& temporary, const std::atomic_bool& cancel, NoteAudioUsage& usage);
void wait_note_audio(int seconds, const std::atomic_bool& cancel, const std::function<void(int)>& wait = {});
NoteAudioError note_audio_codec_error(std::string_view operation, long code);
void encode_note_audio(const std::filesystem::path& wav, const std::filesystem::path& m4a);
}
