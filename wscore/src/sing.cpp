#include "wscore/sing.hpp"
namespace ws {
void Sing::run(std::string six, std::string four) {
    Separator separator;
    int loaded = -1;
    const size_t window = size_t(sourceRate) * 6, stride = size_t(sourceRate) * 5;
    Stereo chunk;
    for (auto& c : chunk) c.reserve(window);
    std::vector<SingFrame> overlap;
    uint64_t seenDrops = 0, chunkStart = 0, expected = 0;
    while (!quit.load()) {
        SingInput f;
        if (input.size() > window) {
            for (auto& c : chunk) c.clear();
            overlap.clear();
            while (input.size() > window && input.pop(f)) dropped.fetch_add(1, std::memory_order_relaxed);
        }
        while (chunk[0].size() < window && input.pop(f)) {
            if (f.position != expected) { for (auto& c : chunk) c.clear(); overlap.clear(); }
            if (chunk[0].empty()) chunkStart = f.position;
            expected = f.position + 1;
            chunk[0].push_back(f.audio.l); chunk[1].push_back(f.audio.r);
        }
        bufferedFrames.store(chunk[0].size());
        if (chunk[0].size() < window) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        processing.store(true);
        auto start = std::chrono::steady_clock::now();
        auto audio = resample(chunk, sourceRate);
        auto level = quality.load();
        Stems stems;
        try {
            if (level < 2) {
                if (loaded != level) { separator.unload(); separator.load(level == 0 ? six : four, level == 0 ? 6 : 4); loaded = level; }
                stems = separator.separate(audio, [&](float) { if (quit.load()) throw std::runtime_error("cancelled");
                    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > 1.5) throw std::runtime_error("live deadline exceeded"); });
            }
        } catch (...) {
            if (quit.load()) return;
            level = level == 0 && !four.empty() ? 1 : 2; quality.store(level);
            separator.unload(); loaded = -1;
            processing.store(false);
            continue;
        }
        std::vector<SingFrame> result(audio[0].size());
        for (size_t i = 0; i < result.size(); ++i) {
            float l = audio[0][i], r = audio[1][i];
            float vl = level < 2 ? stems[0][0][i] : (l + r) * .5f;
            float vr = level < 2 ? stems[0][1][i] : (l + r) * .5f;
            result[i] = {l, r, vl, vr, chunkStart + uint64_t(double(i + 1) * sourceRate / rate)};
        }
        for (size_t i = 0; i < overlap.size() && i < result.size(); ++i) {
            float t = float(i) / overlap.size();
            result[i].vl = overlap[i].vl * (1 - t) + result[i].vl * t;
            result[i].vr = overlap[i].vr * (1 - t) + result[i].vr * t;
        }
        double compute = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        size_t publish = 5 * rate;
        overlap.assign(result.begin() + publish, result.end());
        for (size_t i = 0; i < publish && !quit.load(); ++i) {
            while (!ready.push(result[i]) && !quit.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        processing.store(false);
        rtf.store(compute / 5);
        latency.store(6 + compute + double(input.size()) / sourceRate);
        if ((compute / 5 > .8 || input.size() > sourceRate * 7 || dropped.load() != seenDrops) && level < 2) {
            quality.store(level == 0 && !four.empty() ? 1 : 2);
        }
        seenDrops = dropped.load();
        for (auto& c : chunk) c.erase(c.begin(), c.begin() + stride);
        chunkStart += stride;
        // TODO: ease back up the quality ladder after thermal recovery.
    }
}
void Sing::render(float* left, float* right, unsigned count) noexcept WS_REALTIME {
    std::fill_n(left, count, 0); std::fill_n(right, count, 0);
    if (!primed) { if (ready.size() < rate / 2) { outputReady.store(false); return; } primed = true; }
    outputReady.store(true);
    stretch.setTransposeSemitones(std::clamp(pitch.load(), -6.f, 6.f));
    for (unsigned offset = 0; offset < count;) {
        unsigned n = std::min(256u, count - offset);
        // A small queue servo absorbs independent device clock drift.
        double correction = std::clamp((double(ready.size()) - 2.5 * rate) / (rate * 2500.), -.002, .002);
        fraction += n * (1 + correction);
        unsigned need = unsigned(fraction); fraction -= need;
        float amount = std::clamp(removal.load(), 0.f, 1.f);
        for (unsigned i = 0; i < need; ++i) {
            SingFrame f{};
            if (!ready.pop(f)) { underruns.fetch_add(1, std::memory_order_relaxed); primed = false; outputReady.store(false); }
            else consumed.store(f.position, std::memory_order_relaxed);
            mixed[0][i] = f.l - amount * f.vl; mixed[1][i] = f.r - amount * f.vr;
        }
        float* ins[]{mixed[0].data(), mixed[1].data()};
        float* outs[]{left + offset, right + offset};
        stretch.process(ins, need, outs, n);
        for (unsigned i = offset; i < offset + n; ++i) {
            float peak = std::max(std::abs(left[i]), std::abs(right[i]));
            float target = peak > .98f ? .98f / peak : 1;
            gain = target < gain ? target : gain + .0002f * (target - gain);
            left[i] = std::isfinite(left[i]) ? left[i] * gain : 0;
            right[i] = std::isfinite(right[i]) ? right[i] * gain : 0;
        }
        offset += n;
    }
}
}
