#include "media/note_audio.h"
#include <fstream>
#include <stdexcept>
#include <array>
#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#pragma clang diagnostic pop
#endif

namespace cha {
void encode_note_audio(const std::filesystem::path& wav, const std::filesystem::path& m4a) {
#if defined(__APPLE__) || defined(_WIN32)
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
#endif
#ifdef __APPLE__
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
#elif defined(_WIN32)
    const auto check = [](HRESULT status) {
        if (FAILED(status)) throw std::runtime_error("Native audio conversion failed (" + std::to_string(status) + ").");
    };
    // Generation runs on a worker thread. Balance each successful initialization
    // after all reader/writer COM objects have been released.
    struct Runtime {
        bool com{}, media{};
        ~Runtime() { if (media) MFShutdown(); if (com) CoUninitialize(); }
    } runtime;
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (initialized != RPC_E_CHANGED_MODE) check(initialized);
    runtime.com = SUCCEEDED(initialized);
    check(MFStartup(MF_VERSION));
    runtime.media = true;
    using Microsoft::WRL::ComPtr;
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(wav.c_str(), nullptr, &reader));
    ComPtr<IMFMediaType> native;
    check(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &native));
    GUID subtype{};
    UINT32 rate{}, channels{}, bits{};
    check(native->GetGUID(MF_MT_SUBTYPE, &subtype));
    check(native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate));
    check(native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels));
    check(native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits));
    if (subtype != MFAudioFormat_PCM || rate != 24000 || channels != 1 || bits != 16)
        throw std::runtime_error("Expected a mono PCM16 WAV at 24 kHz.");
    ComPtr<IMFMediaType> pcm;
    check(MFCreateMediaType(&pcm));
    check(pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check(pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM));
    check(pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
    check(pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1));
    check(pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    check(pcm->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 2));
    check(pcm->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 96000));
    // The Source Reader inserts the native audio resampler (Windows 8+).
    check(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm.Get()));
    ComPtr<IMFMediaType> aac;
    check(MFCreateMediaType(&aac));
    check(aac->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio));
    check(aac->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC));
    check(aac->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
    check(aac->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1));
    check(aac->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16));
    check(aac->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 12000));
    ComPtr<IMFSinkWriter> writer;
    check(MFCreateSinkWriterFromURL(m4a.c_str(), nullptr, nullptr, &writer));
    DWORD output{};
    check(writer->AddStream(aac.Get(), &output));
    check(writer->SetInputMediaType(output, pcm.Get(), nullptr));
    check(writer->BeginWriting());
    std::uint64_t frames{};
    for (;;) {
        DWORD flags{};
        ComPtr<IMFSample> sample;
        check(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample));
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
            throw std::runtime_error("The service returned an unexpected audio format change.");
        if (sample) {
            DWORD length{};
            check(sample->GetTotalLength(&length));
            if (length % 2) throw std::runtime_error("The service returned truncated audio.");
            const auto count = length / 2;
            if (count) {
                const auto start = static_cast<LONGLONG>(frames * 10000000 / 48000);
                frames += count;
                check(sample->SetSampleTime(start));
                check(sample->SetSampleDuration(static_cast<LONGLONG>(frames * 10000000 / 48000) - start));
                check(writer->WriteSample(output, sample.Get()));
            }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    if (!frames) throw std::runtime_error("The service returned empty audio.");
    check(writer->Finalize());
#else
    (void)wav; (void)m4a;
    throw std::runtime_error("Note audio generation is available on macOS and Windows only.");
#endif
}
}
