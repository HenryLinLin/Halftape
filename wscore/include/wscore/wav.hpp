#pragma once

#include "audio.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace ws {
struct Wav {
    Stereo audio;
    unsigned sampleRate = 0;
};

namespace detail {
inline uint16_t wavU16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t wavU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline bool wavTag(const uint8_t* p, const char* tag) {
    return p[0] == uint8_t(tag[0]) && p[1] == uint8_t(tag[1]) &&
           p[2] == uint8_t(tag[2]) && p[3] == uint8_t(tag[3]);
}
}

inline Wav readWav(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open WAV file: " + path);
    auto end = file.tellg();
    if (end < 12) throw std::runtime_error("invalid WAV: file is too short");
    auto fileSize = static_cast<uint64_t>(end);
    if (fileSize > std::numeric_limits<size_t>::max()) throw std::runtime_error("WAV is too large");
    std::vector<uint8_t> bytes(static_cast<size_t>(fileSize));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("failed to read WAV file: " + path);

    if (!detail::wavTag(bytes.data(), "RIFF") || !detail::wavTag(bytes.data() + 8, "WAVE"))
        throw std::runtime_error("invalid WAV: expected little-endian RIFF/WAVE");
    const uint64_t riffEnd = uint64_t(detail::wavU32(bytes.data() + 4)) + 8;
    if (riffEnd < 12 || riffEnd > fileSize) throw std::runtime_error("invalid WAV: RIFF size exceeds file bounds");

    bool haveFormat = false, haveData = false;
    uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
    uint32_t sampleRate = 0;
    const uint8_t* audioData = nullptr;
    uint32_t audioBytes = 0;
    uint64_t pos = 12;
    while (pos < riffEnd) {
        if (riffEnd - pos < 8) throw std::runtime_error("invalid WAV: truncated chunk header");
        const uint8_t* header = bytes.data() + pos;
        uint32_t chunkSize = detail::wavU32(header + 4);
        uint64_t body = pos + 8, chunkEnd = body + uint64_t(chunkSize);
        if (chunkEnd > riffEnd) throw std::runtime_error("invalid WAV: chunk exceeds RIFF bounds");
        const uint8_t* chunk = bytes.data() + body;
        if (detail::wavTag(header, "fmt ")) {
            if (haveFormat || chunkSize < 16) throw std::runtime_error("invalid WAV: duplicate or short format chunk");
            format = detail::wavU16(chunk);
            channels = detail::wavU16(chunk + 2);
            sampleRate = detail::wavU32(chunk + 4);
            uint32_t byteRate = detail::wavU32(chunk + 8);
            blockAlign = detail::wavU16(chunk + 12);
            bits = detail::wavU16(chunk + 14);
            if (format == 0xfffe) {
                if (chunkSize < 40 || detail::wavU16(chunk + 16) < 22)
                    throw std::runtime_error("invalid WAV: malformed extensible format chunk");
                uint16_t validBits = detail::wavU16(chunk + 18);
                const uint8_t* guid = chunk + 24;
                const std::array<uint8_t, 12> guidTail{0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
                // KSDATAFORMAT_SUBTYPE_{PCM,IEEE_FLOAT}: Data1 is 1 or 3, followed by this GUID tail.
                bool known = guid[1] == 0 && guid[2] == 0 && guid[3] == 0 &&
                             std::equal(guidTail.begin(), guidTail.end(), guid + 4);
                if (!known || (guid[0] != 1 && guid[0] != 3) || validBits != bits)
                    throw std::runtime_error("unsupported WAV extensible subformat");
                format = guid[0];
            }
            if (format != 1 && format != 3) throw std::runtime_error("unsupported WAV encoding");
            if (channels < 1 || channels > 2 || !sampleRate) throw std::runtime_error("unsupported WAV channel count or sample rate");
            if ((format == 1 && bits != 16 && bits != 24 && bits != 32) || (format == 3 && bits != 32))
                throw std::runtime_error("unsupported WAV sample format");
            uint16_t expectedAlign = uint16_t(channels * (bits / 8));
            if (blockAlign != expectedAlign || uint64_t(sampleRate) * blockAlign != byteRate)
                throw std::runtime_error("invalid WAV: inconsistent format sizes");
            haveFormat = true;
        } else if (detail::wavTag(header, "data")) {
            if (haveData) throw std::runtime_error("invalid WAV: multiple data chunks are unsupported");
            audioData = chunk;
            audioBytes = chunkSize;
            haveData = true;
        }
        pos = chunkEnd + (chunkSize & 1u);
        if (pos > riffEnd) throw std::runtime_error("invalid WAV: missing chunk padding");
    }
    if (!haveFormat || !haveData) throw std::runtime_error("invalid WAV: missing format or data chunk");
    if (audioBytes % blockAlign) throw std::runtime_error("invalid WAV: partial audio frame");

    const size_t frames = audioBytes / blockAlign;
    Wav result;
    result.sampleRate = sampleRate;
    result.audio[0].resize(frames);
    result.audio[1].resize(frames);
    const size_t sampleBytes = bits / 8;
    for (size_t i = 0; i < frames; ++i) {
        for (size_t ch = 0; ch < channels; ++ch) {
            const uint8_t* p = audioData + i * blockAlign + ch * sampleBytes;
            float value;
            if (format == 3) {
                uint32_t raw = detail::wavU32(p);
                value = std::bit_cast<float>(raw);
                if (!std::isfinite(value)) throw std::runtime_error("invalid WAV: non-finite float sample");
            } else if (bits == 16) {
                int16_t raw = static_cast<int16_t>(detail::wavU16(p));
                value = float(raw) / 32768.f;
            } else if (bits == 24) {
                int32_t raw = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
                if (raw & 0x800000) raw |= ~0xffffff;
                value = float(raw) / 8388608.f;
            } else {
                int32_t raw = static_cast<int32_t>(detail::wavU32(p));
                value = float(double(raw) / 2147483648.0);
            }
            result.audio[ch][i] = value;
            if (channels == 1) result.audio[1][i] = value;
        }
    }
    return result;
}
}
