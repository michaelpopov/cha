#include "media/note_audio.h"
#include <fstream>
#include <stdexcept>
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#pragma clang diagnostic pop
#include <array>
#include <memory>
#endif

namespace cha {
void encode_note_audio(const std::filesystem::path& wav, const std::filesystem::path& m4a) {
#ifdef __APPLE__
    // A native reader can accept a truncated RIFF file and return an early EOF.
    std::array<unsigned char, 12> header{};
    std::ifstream stream(wav, std::ios::binary);
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    const auto riff_size = static_cast<std::uint64_t>(header[4])
        | (static_cast<std::uint64_t>(header[5]) << 8)
        | (static_cast<std::uint64_t>(header[6]) << 16)
        | (static_cast<std::uint64_t>(header[7]) << 24);
    if (!stream || std::string_view(reinterpret_cast<const char*>(header.data()), 4) != "RIFF"
        || std::string_view(reinterpret_cast<const char*>(header.data() + 8), 4) != "WAVE"
        || riff_size + 8 != std::filesystem::file_size(wav))
        throw std::runtime_error("The service returned a truncated or invalid WAV.");
    stream.close();
    const auto check = [](OSStatus status) {
        if (status != noErr) throw std::runtime_error("Native audio conversion failed (" + std::to_string(status) + ").");
    };
    const auto url = [](const std::filesystem::path& path) {
        const auto name = path.string();
        return CFURLCreateFromFileSystemRepresentation(nullptr,
            reinterpret_cast<const UInt8*>(name.data()), static_cast<CFIndex>(name.size()), false);
    };
    struct File {
        ExtAudioFileRef value{};
        ~File() { if (value) ExtAudioFileDispose(value); }
    } input, output;
    CFURLRef source = url(wav);
    const auto opened = ExtAudioFileOpenURL(source, &input.value);
    CFRelease(source);
    check(opened);
    AudioStreamBasicDescription format{};
    UInt32 size = sizeof(format);
    check(ExtAudioFileGetProperty(input.value, kExtAudioFileProperty_FileDataFormat, &size, &format));
    if (format.mFormatID != kAudioFormatLinearPCM || format.mSampleRate != 24000
        || format.mChannelsPerFrame != 1 || format.mBitsPerChannel != 16
        || !(format.mFormatFlags & kAudioFormatFlagIsSignedInteger)
        || (format.mFormatFlags & kAudioFormatFlagIsFloat)) {
        throw std::runtime_error("Expected a mono PCM16 WAV at 24 kHz.");
    }
    SInt64 expected_frames{};
    size = sizeof(expected_frames);
    check(ExtAudioFileGetProperty(input.value, kExtAudioFileProperty_FileLengthFrames, &size, &expected_frames));
    AudioStreamBasicDescription pcm{};
    pcm.mSampleRate = 24000;
    pcm.mFormatID = kAudioFormatLinearPCM;
    pcm.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    pcm.mBytesPerPacket = pcm.mBytesPerFrame = 2;
    pcm.mFramesPerPacket = pcm.mChannelsPerFrame = 1;
    pcm.mBitsPerChannel = 16;
    check(ExtAudioFileSetProperty(input.value, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm));
    AudioStreamBasicDescription aac{};
    aac.mSampleRate = 24000;
    aac.mFormatID = kAudioFormatMPEG4AAC;
    aac.mChannelsPerFrame = 1;
    size = sizeof(aac);
    check(AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, nullptr, &size, &aac));
    CFURLRef destination = url(m4a);
    const auto created = ExtAudioFileCreateWithURL(destination, kAudioFileM4AType, &aac, nullptr,
        kAudioFileFlags_EraseFile, &output.value);
    CFRelease(destination);
    check(created);
    check(ExtAudioFileSetProperty(output.value, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm));
    AudioConverterRef converter{};
    size = sizeof(converter);
    check(ExtAudioFileGetProperty(output.value, kExtAudioFileProperty_AudioConverter, &size, &converter));
    UInt32 bitrate = 64000;
    check(AudioConverterSetProperty(converter, kAudioConverterEncodeBitRate, sizeof(bitrate), &bitrate));
    std::array<std::int16_t, 4096> samples{};
    std::uint64_t total = 0;
    for (;;) {
        UInt32 frames = static_cast<UInt32>(samples.size());
        AudioBufferList buffers{1, {{1, static_cast<UInt32>(sizeof(samples)), samples.data()}}};
        check(ExtAudioFileRead(input.value, &frames, &buffers));
        if (frames == 0) break;
        total += frames;
        check(ExtAudioFileWrite(output.value, frames, &buffers));
    }
    if (total == 0 || expected_frames <= 0 || total != static_cast<std::uint64_t>(expected_frames))
        throw std::runtime_error("The service returned empty or truncated audio.");
    const auto finished = ExtAudioFileDispose(output.value);
    output.value = nullptr;
    check(finished);
#else
    (void)wav; (void)m4a;
    throw std::runtime_error("Note audio generation is available on macOS only.");
#endif
}
}
