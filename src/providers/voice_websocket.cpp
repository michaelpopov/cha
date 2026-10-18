#include "providers/voice_output.h"
#include "media/xai_socket.h"
#include "util/logging.h"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <chrono>
#include <memory>

namespace cha {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string encode(std::string_view value) {
    char* raw = curl_easy_escape(nullptr, value.data(), static_cast<int>(value.size()));
    if (!raw) throw std::runtime_error("Could not encode voice settings.");
    std::unique_ptr<char, decltype(&curl_free)> owned(raw, curl_free);
    return raw;
}

std::string audio_bytes(const Json& value) {
    if (!value.is_string()) throw std::runtime_error("Voice WebSocket returned invalid audio.");
    const auto& encoded = value.get_ref<const std::string&>();
    if (encoded.empty()) return {};
    if (encoded.size() % 4 || encoded.size() > 16 * 1024 * 1024)
        throw std::runtime_error("Voice WebSocket returned invalid audio.");
    const auto padding = encoded.ends_with("==") ? 2 : encoded.ends_with('=') ? 1 : 0;
    if (encoded.substr(0, encoded.size() - padding).find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") != std::string::npos)
        throw std::runtime_error("Voice WebSocket returned invalid audio.");
    std::string bytes(encoded.size() / 4 * 3, '\0');
    const int count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()),
        reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
    if (count < padding) throw std::runtime_error("Voice WebSocket returned invalid audio.");
    bytes.resize(static_cast<std::size_t>(count - padding));
    return bytes;
}
}

std::optional<EntryAudio> stream_voice_websocket(
    const WorkspaceVoiceProviderOutput& output, const std::string& key,
    const VoiceOutputRequest& request, const std::function<bool()>& cancelled,
    const AudioChunkCallback& on_audio, const VoiceTextInput& input) {
    return stream_voice_websocket_with_timeout(output, key, request, cancelled, on_audio, input, 180s);
}

std::optional<EntryAudio> stream_voice_websocket_with_timeout(
    const WorkspaceVoiceProviderOutput& output, const std::string& key,
    const VoiceOutputRequest& request, const std::function<bool()>& cancelled,
    const AudioChunkCallback& on_audio, const VoiceTextInput& input,
    std::chrono::milliseconds idle_timeout) {
    const auto started = Clock::now();
    std::optional<Clock::time_point> first_audio, last_audio;
    std::size_t text_bytes = 0, audio_chunks = 0;
    bool text_done = false;
    std::string last_event = "none";
    EntryAudio audio{{}, output.output_format.starts_with("opus") ? "audio/ogg"
        : output.output_format == "wav" ? "audio/wav" : "audio/mpeg"};
    const std::string fields = " provider=" + request.provider + " model=" + output.model
        + " output_format=" + output.output_format + " live_input=" + (input ? "true" : "false");
    // Provider text can be invalid UTF-8. Logging must not turn a result into a failure.
    const auto quoted = [](std::string_view text) {
        return Json(std::string(text)).dump(-1, ' ', false, Json::error_handler_t::replace);
    };
    const auto milliseconds = [](Clock::duration duration) {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
    };
    const auto summarize = [&](std::string_view outcome, std::string_view error = {}) {
        const auto now = Clock::now();
        const std::string message = "Voice WebSocket finished:" + fields
            + " outcome=" + std::string(outcome) + " duration_ms=" + milliseconds(now - started)
            + " text_complete=" + (text_done ? "true" : "false")
            + " text_bytes=" + std::to_string(text_bytes)
            + " audio_chunks=" + std::to_string(audio_chunks)
            + " audio_bytes=" + std::to_string(audio.audio.size())
            + " first_audio_ms=" + (first_audio ? milliseconds(*first_audio - started) : "unreported")
            + " last_audio_ago_ms=" + (last_audio ? milliseconds(now - *last_audio) : "unreported")
            + " last_event=" + quoted(last_event)
            + (error.empty() ? "" : " error=" + quoted(error));
        if (outcome == "failed") log_warn(message);
        else log_info(message);
    };
    log_info("Voice WebSocket started:" + fields + " idle_timeout_ms=" + std::to_string(idle_timeout.count()));
    try {
        if (cancelled()) { summarize("cancelled"); return std::nullopt; }
        const bool fish = request.provider == "fishaudio";
        const bool dialogue = !fish && (output.model.starts_with("eleven_v3") || output.model.starts_with("eleven_v4"));
        auto base = output.url;
        while (base.ends_with('/')) base.pop_back();
        if (base.starts_with("https://")) base.replace(0, 5, "wss");
        else if (base.starts_with("http://")) base.replace(0, 4, "ws");
        std::string url;
        if (fish) url = base + "/live";
        else {
            const auto voice = request.voice_id;
            if (dialogue) {
                const auto path = base.find("/v1/text-to-speech");
                if (path == std::string::npos) throw std::invalid_argument("Invalid ElevenLabs WebSocket endpoint.");
                base.replace(path, std::string::npos, "/v1/text-to-dialogue");
                url = base + "/stream-input";
            } else url = base + "/" + encode(voice) + "/stream-input";
            url += "?model_id=" + encode(output.model) + "&output_format=" + encode(output.output_format);
        }
        auto socket = media::make_voice_curl_socket(fish ? std::vector<std::string>{"model: " + output.model} : std::vector<std::string>{});
        struct Close { media::XaiSocket& socket; ~Close() { socket.close(); } } close{*socket};
        socket->connect(url, fish ? "Authorization: Bearer " + key : "xi-api-key: " + key,
            Clock::now() + 10s, cancelled);
        log_debug("Voice WebSocket connected:" + fields + " duration_ms=" + milliseconds(Clock::now() - started));
        auto last_send = Clock::now();
        const auto send = [&](const Json& message) {
            std::string payload;
            if (fish) {
                const auto bytes = Json::to_msgpack(message);
                payload.assign(bytes.begin(), bytes.end());
            } else payload = message.dump();
            socket->send(payload, fish, Clock::now() + 10s, cancelled);
            last_send = Clock::now();
        };
        const auto send_text = [&](std::string_view text, bool flush) {
            if (text.empty()) return;
            if (fish) {
                send({{"event", "text"}, {"text", text}});
                if (flush) send({{"event", "flush"}});
            } else if (dialogue) send({{"inputs", Json::array({{{"text", text},
                {"voice_id", request.voice_id}, {"new_turn", false}}})}, {"flush", flush}});
            else send({{"text", text}, {"flush", flush}});
            text_bytes += text.size();
        };
        if (fish) {
            auto settings = request.body;
            settings["text"] = "";
            send({{"event", "start"}, {"request", settings}});
        } else if (dialogue) {
            send({{"voices", Json::array({request.voice_id})}});
        } else {
            Json init{{"text", " "}};
            if (request.body.contains("voice_settings")) init["voice_settings"] = request.body["voice_settings"];
            send(init);
        }
        auto receive_deadline = Clock::time_point::max();
        while (!cancelled()) {
            if (!text_done) {
                const auto chunk = input ? input() : VoiceTextChunk{request.body.at("text").get<std::string>(), true};
                const std::string_view text = chunk.text;
                const auto end = text.find_last_of(".!?\n");
                if (end != std::string_view::npos) {
                    send_text(text.substr(0, end + 1), true);
                    send_text(text.substr(end + 1), false);
                } else send_text(text, false);
                if (chunk.complete) {
                    if (fish) send({{"event", "stop"}});
                    else if (dialogue) send({{"close_socket", true}});
                    else send({{"text", ""}});
                    text_done = true;
                    receive_deadline = Clock::now() + idle_timeout;
                    log_debug("Voice WebSocket text completed:" + fields
                        + " text_bytes=" + std::to_string(text_bytes)
                        + " duration_ms=" + milliseconds(Clock::now() - started));
                } else if (!fish && Clock::now() - last_send >= 10s) {
                    // ElevenLabs closes idle input after 20 seconds, including LLM pauses.
                    if (dialogue) send({{"keep_alive", true}});
                    else send({{"text", " "}});
                }
            }
            if (Clock::now() >= receive_deadline) throw std::runtime_error("Voice WebSocket timed out waiting for audio.");
            auto incoming = socket->recv(50ms);
            if (!incoming) continue;
            if (incoming->closed) throw std::runtime_error("Voice WebSocket disconnected before synthesis completed.");
            if (incoming->binary != fish) throw std::runtime_error("Voice WebSocket returned an invalid message.");
            auto message = fish ? Json::from_msgpack(incoming->payload) : Json::parse(incoming->payload);
            last_event = fish ? message.value("event", std::string("unknown"))
                : message.contains("audio") ? "audio" : "control";
            if (message.contains("error") && !message["error"].is_null()) {
                log_debug_payload("Voice WebSocket provider error:" + fields, message["error"].dump(), key);
                throw std::runtime_error("Voice WebSocket synthesis failed. Check the model, voice and API key.");
            }
            std::string bytes;
            bool finished = false;
            if (fish) {
                const auto event = message.value("event", std::string());
                if (event == "audio") {
                    const auto& data = message.at("audio");
                    if (!data.is_binary()) throw std::runtime_error("FishAudio returned invalid audio.");
                    const auto& binary = data.get_binary();
                    bytes.assign(binary.begin(), binary.end());
                } else if (event == "finish") {
                    log_debug_payload("Voice WebSocket provider finish:" + fields, message.value("reason", std::string()), key);
                    if (message.value("reason", std::string()) != "stop") throw std::runtime_error("FishAudio WebSocket synthesis failed.");
                    finished = true;
                }
            } else {
                if (message.contains("audio") && !message["audio"].is_null()) bytes = audio_bytes(message["audio"]);
                const auto final_field = dialogue ? "is_final" : "isFinal";
                finished = message.contains(final_field) && message[final_field] == true;
            }
            if (!bytes.empty()) {
                if (bytes.size() > 256 * 1024 * 1024 - audio.audio.size()) throw std::runtime_error("Audio is too large.");
                last_audio = Clock::now();
                // Once input is complete, wait for audio inactivity, not total synthesis time.
                if (text_done) receive_deadline = *last_audio + idle_timeout;
                ++audio_chunks;
                audio.audio += bytes;
                if (!first_audio) {
                    first_audio = last_audio;
                    log_info("Voice WebSocket first audio:" + fields
                        + " duration_ms=" + milliseconds(*first_audio - started));
                }
                if (on_audio && !cancelled()) on_audio(audio.content_type, bytes);
            }
            if (finished) {
                if (!text_done) throw std::runtime_error("Voice WebSocket completed before the answer finished.");
                if (!valid_entry_audio(audio)) throw std::runtime_error("Voice provider returned invalid audio.");
                const bool stopped = cancelled();
                summarize(stopped ? "cancelled" : "completed");
                return stopped ? std::nullopt : std::optional<EntryAudio>(std::move(audio));
            }
        }
        summarize("cancelled");
        return std::nullopt;
    } catch (const Json::exception&) {
        summarize("failed", "Voice WebSocket returned an invalid message.");
        throw std::runtime_error("Voice WebSocket returned an invalid message.");
    } catch (const media::XaiVoiceFailure& error) {
        if (cancelled()) { summarize("cancelled"); return std::nullopt; }
        summarize("failed", error.what());
        if (error.what() == media::xai_authentication_failed)
            throw std::runtime_error("Voice WebSocket authentication failed. Check the API key.");
        if (error.what() == media::xai_configuration_rejected)
            throw std::runtime_error("Voice WebSocket configuration was rejected. Check the model and voice.");
        if (error.what() == media::xai_websocket_required)
            throw std::runtime_error("Voice output requires a curl build with WebSocket support.");
        throw std::runtime_error("Voice WebSocket connection failed.");
    } catch (const std::exception& error) {
        summarize("failed", error.what());
        throw;
    }
}
}
