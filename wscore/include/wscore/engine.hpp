#pragma once
#include "audio.hpp"
#include "realtime.hpp"
#include "separation.hpp"
#include "signalsmith-stretch.h"
namespace ws {
struct Playback {
    std::shared_ptr<const Clip> clip;
    std::shared_ptr<StemBank> stems;
    std::unique_ptr<float[]> blend;
    Playback(std::shared_ptr<const Clip> clip, std::shared_ptr<StemBank> stems = {})
        : clip(std::move(clip)), stems(std::move(stems)), blend(this->stems ? std::make_unique<float[]>(this->stems->size) : nullptr) {}
};
class Engine {
    std::atomic<Playback*> published{nullptr}, hazard{nullptr};
    std::vector<std::unique_ptr<Playback>> owned;
    uint64_t lastID = 0;
    signalsmith::stretch::SignalsmithStretch<float> stretch{42};
    std::array<std::array<float, 384>, 2> input{};
    double remainder = 0;
    uint32_t position = 0;
    float limitGain = 1;
    uint64_t countRemaining = 0, countTotal = 0;
    std::array<float, stemCount> smooth{1, 1, 1, 1, 1, 1};
public:
    std::array<std::atomic<float>, stemCount> gains;
    std::atomic<bool> playing{false}, trainerEnabled{false}, countRequested{false};
    std::atomic<float> trainerStep{.05f};
    std::atomic<unsigned> trainerEvery{2};
    std::atomic<float> speed{1}, pitch{0};
    std::atomic<uint64_t> loop{0};
    std::atomic<double> playhead{0};
    std::atomic<uint64_t> loops{0};
    Engine() { stretch.presetDefault(2, rate); for (auto& gain : gains) gain.store(1); }
    void publish(std::unique_ptr<Playback> next) {
        auto* p = next.get(); owned.push_back(std::move(next));
        published.store(p, std::memory_order_seq_cst); collect();
    }
    void collect() {
        auto* protectedClip = hazard.load(std::memory_order_seq_cst);
        auto* active = published.load(std::memory_order_seq_cst);
        std::erase_if(owned, [&](const auto& x) { return x.get() != protectedClip && x.get() != active; });
    }
    void setLoop(double a, double b) {
        uint32_t start = uint32_t(std::max(0., a) * rate), end = uint32_t(std::max(0., b) * rate);
        loop.store(uint64_t(start) << 32 | end, std::memory_order_release);
    }
    void render(float* left, float* right, unsigned count) noexcept WS_REALTIME {
        Playback* data;
        do { data = published.load(std::memory_order_seq_cst); hazard.store(data, std::memory_order_seq_cst); }
        while (data != published.load(std::memory_order_seq_cst));
        if (!data || !playing.load(std::memory_order_relaxed) || data->clip->audio[0].empty()) {
            std::fill_n(left, count, 0); std::fill_n(right, count, 0);
            hazard.store(nullptr, std::memory_order_seq_cst); return;
        }
        auto size = uint32_t(data->clip->audio[0].size());
        auto range = loop.load(std::memory_order_acquire);
        auto a = std::min(uint32_t(range >> 32), size - 1);
        auto b = std::clamp(uint32_t(range), a + 1, size);
        if (lastID != data->clip->id) { stretch.reset(); lastID = data->clip->id; position = a; remainder = 0; loops.store(0); }
        if (position < a || position >= b) position = a;
        float ratio = std::clamp(speed.load(std::memory_order_relaxed), .25f, 1.5f);
        float transpose = std::clamp(pitch.load(std::memory_order_relaxed), -13.f, 13.f);
        stretch.setTransposeSemitones(transpose);
        double beatSamples = 60 * rate / std::max(30., data->clip->bpm > 0 ? data->clip->bpm : 120.) / ratio;
        if (countRequested.exchange(false)) countRemaining = countTotal = uint64_t(beatSamples * 4);
        if (countRemaining) {
            for (unsigned i = 0; i < count; ++i) {
                double phase = std::fmod(double(countTotal - countRemaining), beatSamples);
                float click = countRemaining && phase < rate * .04 ? float(.2 * std::sin(phase * 6.283185307 * 1200 / rate) * std::exp(-phase / 350)) : 0;
                left[i] = right[i] = click;
                if (countRemaining) --countRemaining;
            }
            hazard.store(nullptr, std::memory_order_seq_cst); return;
        }
        for (unsigned offset = 0; offset < count;) {
            unsigned n = std::min(256u, count - offset);
            remainder += n * double(ratio);
            unsigned needed = unsigned(remainder); remainder -= needed;
            auto sample = [&](unsigned frame, int channel) {
                if (data->stems) {
                    auto* block = data->stems->blocks[frame / segmentFrames].load(std::memory_order_acquire);
                    if (block) {
                        unsigned index = frame / segmentFrames, local = frame % segmentFrames;
                        auto mix = [&](const StemBlock* source, size_t at) {
                            float value = 0;
                            for (unsigned s = 0; s < stemCount; ++s) value += source->audio[s][channel][at] * smooth[s];
                            return value;
                        };
                        float sum = mix(block, local + block->offset);
                        if (index && local < rate * 9 / 10) {
                            auto* previous = data->stems->blocks[index - 1].load(std::memory_order_acquire);
                            size_t at = segmentFrames + local + (previous ? previous->offset : 0);
                            if (previous && at < previous->audio[0][0].size()) {
                                float t = float(local) / (rate * .9f);
                                sum = sum * t + mix(previous, at) * (1 - t);
                            }
                        }
                        float blend = data->blend[index];
                        return sum * blend + data->clip->audio[channel][frame] * (1 - blend);
                    }
                }
                return data->clip->audio[channel][frame];
            };
            unsigned fade = std::min(441u, (b - a) / 4);
            for (unsigned i = 0; i < needed; ++i) {
                auto reveal = [&](unsigned frame) {
                    if (!data->stems) return;
                    auto index = frame / segmentFrames;
                    if (data->stems->blocks[index].load(std::memory_order_acquire)) data->blend[index] = std::min(1.f, data->blend[index] + .002f);
                };
                reveal(position);
                if (fade && position >= b - fade) reveal(a + position - (b - fade));
                for (unsigned s = 0; s < stemCount; ++s) smooth[s] += .002f * (gains[s].load(std::memory_order_relaxed) - smooth[s]);
                for (int c = 0; c < 2; ++c) {
                    float value = sample(position, c);
                    if (fade && position >= b - fade) {
                        float t = float(position - (b - fade)) / fade;
                        value = value * std::cos(t * 1.570796327f) + sample(a + position - (b - fade), c) * std::sin(t * 1.570796327f);
                    }
                    input[c][i] = value;
                }
                if (++position >= b) {
                    position = a + fade;
                    auto completed = loops.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (trainerEnabled.load(std::memory_order_relaxed) && completed % std::max(1u, trainerEvery.load()) == 0)
                        speed.store(std::min(1.f, speed.load() + trainerStep.load()), std::memory_order_relaxed);
                }
            }
            float* ins[]{input[0].data(), input[1].data()};
            float* outs[]{left + offset, right + offset};
            stretch.process(ins, needed, outs, n);
            for (unsigned i = offset; i < offset + n; ++i) {
                float peak = std::max(std::abs(left[i]), std::abs(right[i]));
                float desired = peak > .98f ? .98f / peak : 1.f;
                limitGain = desired < limitGain ? desired : limitGain + .0002f * (desired - limitGain);
                left[i] = std::isfinite(left[i]) ? left[i] * limitGain : 0;
                right[i] = std::isfinite(right[i]) ? right[i] * limitGain : 0;
            }
            offset += n;
        }
        playhead.store(double(position) / rate, std::memory_order_relaxed);
        hazard.store(nullptr, std::memory_order_seq_cst);
    }
};
}
