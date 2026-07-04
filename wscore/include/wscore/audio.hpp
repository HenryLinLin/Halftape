#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace ws {
constexpr unsigned rate = 44100;
constexpr unsigned stemCount = 6;
struct Frame { float l = 0, r = 0; };
using Stereo = std::array<std::vector<float>, 2>;
struct Clip {
    uint64_t id = 0;
    Stereo audio;
    std::vector<float> peaks;
    std::vector<double> beats;
    double bpm = 0;
    int key = -1;
    double seconds() const { return double(audio[0].size()) / rate; }
};

template<typename T, size_t Capacity> class Spsc {
    static_assert((Capacity & (Capacity - 1)) == 0);
    std::unique_ptr<T[]> data = std::make_unique<T[]>(Capacity);
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
public:
    bool push(const T& value) noexcept {
        auto h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) == Capacity) return false;
        data[h & (Capacity - 1)] = value;
        head.store(h + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept {
        auto t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire)) return false;
        value = data[t & (Capacity - 1)];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }
    size_t size() const noexcept { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
};

class CaptureStore {
    std::vector<std::array<int16_t, 2>> data;
    size_t cursor = 0, used = 0;
    unsigned sampleRate;
    mutable std::mutex mutex;
public:
    explicit CaptureStore(unsigned sampleRate = 48000, unsigned seconds = 180)
        : data(size_t(sampleRate) * seconds), sampleRate(sampleRate) {
        if (!sampleRate || !seconds) throw std::invalid_argument("empty capture store");
    }
    void append(const Frame* frames, size_t count) {
        std::lock_guard lock(mutex);
        for (size_t i = 0; i < count; ++i) {
            auto quantize = [](float v) { return int16_t(std::clamp(std::isfinite(v) ? v : 0.f, -1.f, 1.f) * 32767); };
            data[cursor] = {quantize(frames[i].l), quantize(frames[i].r)};
            cursor = (cursor + 1) % data.size();
            used = std::min(used + 1, data.size());
        }
    }
    Stereo grab(double seconds) const {
        std::lock_guard lock(mutex);
        auto n = std::min(used, size_t(std::clamp(seconds, 0., 180.) * sampleRate));
        Stereo result;
        for (auto& ch : result) ch.resize(n);
        auto start = (cursor + data.size() - n) % data.size();
        for (size_t i = 0; i < n; ++i) for (int c = 0; c < 2; ++c)
            result[c][i] = data[(start + i) % data.size()][c] / 32768.f;
        return result;
    }
    double buffered() const { std::lock_guard lock(mutex); return double(used) / sampleRate; }
    unsigned sample_rate() const { return sampleRate; }
};

inline void trim(Stereo& audio) {
    auto n = audio[0].size();
    size_t a = 0, b = n;
    auto quiet = [&](size_t i) { return std::max(std::abs(audio[0][i]), std::abs(audio[1][i])) < 0.0003f; };
    while (a < b && quiet(a)) ++a;
    while (b > a && quiet(b - 1)) --b;
    a = a > 220 ? a - 220 : 0;
    b = std::min(n, b + 220);
    if (std::all_of(audio[0].begin(), audio[0].end(), [](float x) { return std::abs(x) < .0003f; }) &&
        std::all_of(audio[1].begin(), audio[1].end(), [](float x) { return std::abs(x) < .0003f; })) { audio = {}; return; }
    for (auto& ch : audio) ch = std::vector<float>(ch.begin() + a, ch.begin() + b);
}
inline void waveform(Clip& clip, size_t bins = 960) {
    auto n = clip.audio[0].size();
    clip.peaks.assign(std::min(bins, n), 0);
    for (size_t i = 0; i < n; ++i) {
        auto b = i * clip.peaks.size() / n;
        clip.peaks[b] = std::max(clip.peaks[b], std::max(std::abs(clip.audio[0][i]), std::abs(clip.audio[1][i])));
    }
}
}
