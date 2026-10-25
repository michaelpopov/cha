#include "media/note_audio.h"
#include "util/base64.h"
#include "util/curl.h"
#include "util/text.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <thread>

namespace cha {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_wav = 32 * 1024 * 1024;
constexpr std::size_t max_line = 4 * ((max_wav + 2) / 3) + 65536;

class TransferError : public std::runtime_error {
public:
    TransferError(std::string message, bool retry) : std::runtime_error(std::move(message)), retryable(retry) {}
    bool retryable;
};
struct Transfer {
    std::string body, upload_url;
    std::ofstream* file{};
    std::size_t size{}, limit{};
    const std::atomic_bool* cancel{};
    bool local_failure{};
};
std::size_t receive(char* data, std::size_t width, std::size_t count, void* opaque) noexcept {
    auto& t = *static_cast<Transfer*>(opaque);
    const auto bytes = width * count;
    try {
        if (bytes > t.limit - t.size) throw std::runtime_error("limit");
        if (t.file) {
            t.file->write(data, static_cast<std::streamsize>(bytes));
            if (!*t.file) throw std::runtime_error("write");
        } else t.body.append(data, bytes);
        t.size += bytes;
        return bytes;
    } catch (...) { t.local_failure = true; return 0; }
}
std::size_t header(char* data, std::size_t width, std::size_t count, void* opaque) noexcept {
    auto& t = *static_cast<Transfer*>(opaque);
    try {
        std::string_view line(data, width * count);
        const auto colon = line.find(':');
        if (colon != std::string_view::npos && fold_ascii(line.substr(0, colon)) == "x-goog-upload-url")
            t.upload_url = trim_view(line.substr(colon + 1));
        return width * count;
    } catch (...) { t.local_failure = true; return 0; }
}
int cancelled(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<Transfer*>(opaque)->cancel->load() ? 1 : 0;
}
Transfer request(const NoteAudioService& service, const std::string& url, const std::atomic_bool& cancel,
    const std::vector<std::string>& headers = {}, const std::string* body = nullptr,
    std::ofstream* file = nullptr, std::size_t limit = 65536, bool authenticate = true, bool ambiguous = false) {
    CurlHandle curl;
    CurlHeaders fields;
    if (authenticate) fields.append("x-goog-api-key: " + service.key);
    for (const auto& h : headers) fields.append(h);
    Transfer t; t.file = file; t.limit = limit; t.cancel = &cancel;
    auto* c = curl.get();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, fields.get());
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, cancelled);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &t);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body->data());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
    }
    const auto result = curl_easy_perform(c);
    if (cancel.load()) throw std::runtime_error("Audio generation was interrupted.");
    if (t.local_failure) throw std::runtime_error("Could not receive audio results within the size limit. Check available disk space.");
    if (result != CURLE_OK) throw TransferError(ambiguous
        ? "Submission status unknown. Starting again may generate audio twice."
        : "The audio service transfer failed. Check the service and try again.", true);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) throw TransferError("Audio service returned HTTP " + std::to_string(status)
        + ". Check the service and its API key.", status == 500);
    return t;
}
std::string origin(std::string value) {
    while (value.ends_with('/')) value.pop_back();
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
    if (!url || curl_url_set(url.get(), CURLUPART_URL, value.c_str(), 0) != CURLUE_OK)
        throw std::runtime_error("Set an absolute audio service URL in Notes settings.");
    auto part = [&](CURLUPart p) { char* raw{}; curl_url_get(url.get(), p, &raw, 0);
        std::string result = raw ? raw : ""; curl_free(raw); return result; };
    if ((part(CURLUPART_SCHEME) != "http" && part(CURLUPART_SCHEME) != "https")
        || part(CURLUPART_HOST).empty() || part(CURLUPART_PATH) != "/"
        || !part(CURLUPART_QUERY).empty() || !part(CURLUPART_FRAGMENT).empty()
        || !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty())
        throw std::runtime_error("Use an audio service origin without a path, credentials, query, or fragment.");
    return value;
}
std::string resource(const Json& value, const char* field, std::string_view prefix) {
    const auto name = value.at(field).get<std::string>();
    if (!name.starts_with(prefix) || name.size() != prefix.size() + 32
        || name.substr(prefix.size()).find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::runtime_error("The audio service returned an invalid resource name. Check the service.");
    return name;
}
std::optional<std::size_t> count(const Json& stats, const char* field) {
    if (!stats.is_object()) return {};
    const auto found = stats.find(field);
    if (found == stats.end() || !found->is_string()) return {};
    const auto& text = found->get_ref<const std::string&>();
    std::size_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return {};
    return value;
}
}

std::string prepare_note_audio(NoteAudioService& service, const std::vector<std::string>& blocks) {
    if (blocks.empty()) throw std::runtime_error("No text to narrate");
    if (blocks.size() > 1000) throw std::runtime_error("The note exceeds the 1,000-request batch limit.");
    service.url = origin(service.url);
    if (service.key.empty() || service.key.find_first_of("\r\n") != std::string::npos)
        throw std::runtime_error("Select an audio API key in Notes settings.");
    if (service.voice != "boroda" && service.voice != "grant" && service.voice != "dzig"
        && service.voice != "gerdt" && service.voice != "Kore") throw std::runtime_error("Select an audio voice.");
    std::string jsonl;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto chars = std::count_if(blocks[i].begin(), blocks[i].end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
        if (chars < 1 || chars > 5000) throw std::runtime_error("Text block " + std::to_string(i + 1) + " must contain 1–5,000 Unicode code points.");
        Json item = {{"key", std::to_string(i)}, {"request", {
            {"contents", Json::array({{{"role", "user"}, {"parts", Json::array({{{"text", blocks[i]}}})}}})},
            {"generationConfig", {{"responseModalities", Json::array({"AUDIO"})},
                {"speechConfig", {{"voiceConfig", {{"voice", service.voice}}}}}}}}}};
        jsonl += item.dump() + '\n';
        if (jsonl.size() > 8 * 1024 * 1024) throw std::runtime_error("The note exceeds the 8 MiB JSONL batch limit.");
    }
    return jsonl;
}

std::vector<std::filesystem::path> generate_note_audio(const NoteAudioService& service,
    const std::string& jsonl, std::size_t block_count, const std::filesystem::path& temporary,
    const std::atomic_bool& cancel, const NoteAudioProgress& progress, const std::function<void(int)>& wait) {
    const auto pause = [&](int seconds) {
        if (cancel.load()) throw std::runtime_error("Audio generation was interrupted.");
        if (wait) wait(seconds);
        else for (int i = 0; i < seconds * 10 && !cancel.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (cancel.load()) throw std::runtime_error("Audio generation was interrupted.");
    };
    const auto read = [&](const auto& operation) {
        for (int retry = 0;; ++retry) {
            try { return operation(); }
            catch (const TransferError& error) {
                if (!error.retryable || retry == 3) throw;
                pause(10 << retry);
            }
        }
    };
    progress("uploading", {}, {});
    const std::string metadata = "{}";
    const auto upload = request(service, service.url + "/upload/v1beta/files", cancel,
        {"Content-Type: application/json", "X-Goog-Upload-Protocol: resumable", "X-Goog-Upload-Command: start",
         "X-Goog-Upload-Header-Content-Type: application/jsonl",
         "X-Goog-Upload-Header-Content-Length: " + std::to_string(jsonl.size())}, &metadata);
    // request() restricts the returned URL to HTTP(S), without origin matching.
    if (upload.upload_url.empty())
        throw std::runtime_error("The service returned an invalid upload URL. Check its public URL setting.");
    const auto uploaded = Json::parse(request(service, upload.upload_url, cancel,
        {"Content-Type: application/jsonl", "X-Goog-Upload-Offset: 0", "X-Goog-Upload-Command: upload, finalize"},
        &jsonl, nullptr, 65536, false).body);
    const std::string input = resource(uploaded.at("file"), "name", "files/");
    const std::string body = Json({{"batch", {{"inputConfig", {{"fileName", input}}}}}}).dump();
    auto operation = Json::parse(request(service, service.url + "/v1beta/models/gemini-3.8-flash-tts:batchGenerateContent",
        cancel, {"Content-Type: application/json"}, &body, nullptr, 65536, true, true).body);
    const auto name = resource(operation, "name", "batches/");
    while (!operation.value("done", false)) {
        const auto& info = operation.at("metadata");
        const auto stats = info.value("batchStats", Json::object());
        const auto successful = count(stats, "successfulRequestCount"), failed = count(stats, "failedRequestCount");
        std::optional<std::size_t> completed;
        if (successful && failed && *successful <= std::numeric_limits<std::size_t>::max() - *failed)
            completed = *successful + *failed;
        progress(info.value("state", "") == "BATCH_STATE_PENDING" ? "queued" : "generating",
            completed, count(stats, "requestCount"));
        pause(note_audio_poll_seconds);
        operation = Json::parse(read([&] { return request(service, service.url + "/v1beta/" + name, cancel); }).body);
    }
    if (operation.contains("error")) throw std::runtime_error("The audio batch failed. Check the service before trying again.");
    const auto output = resource(operation.at("response"), "responsesFile", "files/");
    progress("downloading", {}, {});
    const auto result_path = temporary / "results.jsonl";
    read([&] {
        std::ofstream file(result_path, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Could not create the audio download file. Check available disk space.");
        request(service, service.url + "/download/v1beta/" + output + ":download?alt=media", cancel,
            {}, nullptr, &file, block_count * max_line);
        file.close();
        if (!file) throw std::runtime_error("Could not save the audio download. Check available disk space.");
        return true;
    });
    std::ifstream input_file(result_path, std::ios::binary);
    if (!input_file) throw std::runtime_error("Could not open the audio results. Check available disk space.");
    std::vector<std::filesystem::path> clips(block_count);
    std::size_t converted = 0;
    progress("saving", {}, {});
    for (;;) {
        if (cancel.load()) throw std::runtime_error("Audio generation was interrupted.");
        std::string line;
        char c;
        while (input_file.get(c) && c != '\n') {
            if (line.size() == max_line) throw std::runtime_error("An audio result exceeds the clip limit. Check the service.");
            line += c;
        }
        if (input_file.bad()) throw std::runtime_error("Could not read the audio results. Check available disk space.");
        if (line.empty() && input_file.eof()) break;
        const auto item = Json::parse(line);
        const auto key = item.at("key").get<std::string>();
        std::size_t index{};
        const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), index);
        if (error != std::errc{} || end != key.data() + key.size() || index >= clips.size()
            || key != std::to_string(index) || !clips[index].empty())
            throw std::runtime_error("The service returned duplicate or unexpected audio results. Check the service.");
        const auto block = "text block " + std::to_string(index + 1);
        if (item.contains("error")) throw std::runtime_error("Audio generation failed for " + block + ". Check the service.");
        const auto& data = item.at("response").at("candidates").at(0).at("content").at("parts").at(0).at("inlineData");
        if (data.at("mimeType") != "audio/wav") throw std::runtime_error("Expected WAV audio for " + block + ". Check the service.");
        std::string wav;
        try { wav = decode_base64(data.at("data").get_ref<const std::string&>()); }
        catch (const std::runtime_error&) { throw std::runtime_error("Invalid Base64 audio for " + block + ". Check the service."); }
        if (wav.size() > max_wav || wav.size() < 44 || wav.substr(0, 4) != "RIFF" || wav.substr(8, 4) != "WAVE")
            throw std::runtime_error("Invalid WAV audio for " + block + ". Check the service.");
        const auto source = temporary / "clip.wav";
        { std::ofstream file(source, std::ios::binary); file.write(wav.data(), static_cast<std::streamsize>(wav.size()));
          file.close(); if (!file) throw std::runtime_error("Could not save WAV audio. Check available disk space."); }
        clips[index] = temporary / (key + ".m4a");
        try { encode_note_audio(source, clips[index]); }
        catch (const std::exception&) { throw std::runtime_error("Could not convert audio for " + block + ". Check the service's WAV output and available disk space."); }
        std::filesystem::remove(source);
        ++converted;
    }
    if (converted != block_count) throw std::runtime_error("The service returned an incomplete audio batch. Check the service.");
    return clips;
}
}
