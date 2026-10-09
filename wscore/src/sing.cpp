#include "wscore/sing.hpp"
#include "wscore/resample.hpp"
#include <pthread.h>

namespace ws {
Sing::Sing(unsigned sampleRate, const std::string& model) : Sing(sampleRate, openLiveSeparator(model)) {}
// Block separators pause the worker for each model run, so they get one more hop of cushion.
Sing::Sing(unsigned sampleRate, std::unique_ptr<LiveSeparator> processor)
    : separator(std::move(processor)), sourceRate(sampleRate), targetFrames(separator && separator->latency() > 4 * liveHop ? 5120 : 4096) {
    if (sampleRate < 8000 || sampleRate > 192000 || !separator) throw std::runtime_error("Unsupported live audio format");
    stretch.presetDefault(2, rate);
    worker = std::thread([this] { run(); });
}
Sing::~Sing() { quit.store(true); wake.release(); if (worker.joinable()) worker.join(); }
void Sing::feed(Frame frame) noexcept {
    auto position = captured.fetch_add(1, std::memory_order_relaxed);
    if ((position & 4095) == 0) lastFeed.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    if (std::max(std::abs(frame.l), std::abs(frame.r)) > .0001f) lastAudible.store(position + 1, std::memory_order_relaxed);
    if (!input.push({frame, position})) dropped.fetch_add(1, std::memory_order_relaxed);
    if ((position & 127) == 127) wake.release();
}
double Sing::delay() const noexcept {
    double queuedDelay = std::max(0., (double(captured.load()) - double(consumed.load())) / sourceRate);
    return queuedDelay + (std::abs(pitch.load()) > .001f ? double(stretch.inputLatency() + stretch.outputLatency()) / rate : 0);
}
int Sing::state() const noexcept {
    if (failed.load()) return 5;
    auto signal = lastAudible.load();
    if (outputReady.load() && signal && double(consumed.load()) < double(signal) + sourceRate) return 3;
    auto elapsed = std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(lastFeed.load());
    if (!signal || elapsed > std::chrono::seconds(1) || double(captured.load()) > double(signal) + sourceRate) return 4;
    return rtf.load() > 1 ? 2 : 1;
}
std::string Sing::error() { std::lock_guard lock(errorMutex); return failure; }
void Sing::run() {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    try {
        r8b::CDSPResampler leftSRC(sourceRate, rate, 256, 4., 180.15, r8b::fprMinPhase);
        r8b::CDSPResampler rightSRC(sourceRate, rate, 256, 4., 180.15, r8b::fprMinPhase);
        std::array<double, 256> l{}, r{};
        LiveBlock hop{}; LiveStems stems{};
        unsigned fill = 0;
        uint64_t expected = 0, origin = 0, resampled = 0, generation = 0;
        unsigned overruns = 0;
        auto lastOverrun = std::chrono::steady_clock::time_point{};
        auto reset = [&](uint64_t at) {
            separator->reset(); leftSRC.clear(); rightSRC.clear(); fill = 0; origin = at; resampled = 0;
            generation = epoch.fetch_add(1) + 1;
        };
        auto processSample = [&](float a, float b) {
            hop[fill++] = {a, b}; ++resampled;
            if (fill < liveHop) return;
            fill = 0;
            auto start = std::chrono::steady_clock::now();
            timespec cpuStart{}, cpuEnd{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuStart);
            bool valid = separator->process(hop, stems);
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuEnd);
            double cpu = cpuEnd.tv_sec - cpuStart.tv_sec + (cpuEnd.tv_nsec - cpuStart.tv_nsec) * 1e-9;
            cpuRtf.store(.95 * cpuRtf.load() + .05 * cpu * rate / liveHop);
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            maxHopSeconds.store(std::max(maxHopSeconds.load(), elapsed));
            double factor = elapsed * rate / liveHop;
            rtf.store(.95 * rtf.load() + .05 * factor);
            if (!valid) return;
            auto first = resampled - separator->latency();
            for (unsigned i = 0; i < liveHop; ++i) {
                Frame mix{};
                for (auto& stem : stems) { mix.l += stem[i].l; mix.r += stem[i].r; }
                auto position = origin + uint64_t(double(first + i + 1) * sourceRate / rate);
                SingFrame frame{mix.l, mix.r, stems[2][i].l, stems[2][i].r, position, generation};
                while (!ready.push(frame) && !quit.load()) std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
            queued.store(unsigned(ready.size()));
        };
        while (!quit.load()) {
            wake.acquire();
            if (quit.load()) break;
            SingInput frame;
            if (input.size() > sourceRate / 4) {
                auto now = std::chrono::steady_clock::now();
                overruns = now - lastOverrun < std::chrono::seconds(5) ? overruns + 1 : 1;
                lastOverrun = now;
                if (overruns >= 3) throw std::runtime_error("Live separation cannot keep up. Close other busy apps and try again.");
                while (input.size() > sourceRate / 50 && input.pop(frame)) dropped.fetch_add(1, std::memory_order_relaxed);
            }
            unsigned n = 0;
            while (n < l.size() && input.pop(frame)) {
                if (frame.position != expected) { n = 0; reset(frame.position); }
                expected = frame.position + 1;
                l[n] = frame.audio.l; r[n] = frame.audio.r; ++n;
            }
            if (!n) continue;
            if (sourceRate == rate) {
                for (unsigned i = 0; i < n && !quit.load(); ++i) processSample(float(l[i]), float(r[i]));
            } else {
                double *outL = nullptr, *outR = nullptr;
                int count = leftSRC.process(l.data(), n, outL);
                if (rightSRC.process(r.data(), n, outR) != count) throw std::runtime_error("Live resampler channels lost alignment");
                for (int i = 0; i < count && !quit.load(); ++i) processSample(float(outL[i]), float(outR[i]));
            }
        }
    } catch (const std::exception& e) {
        { std::lock_guard lock(errorMutex); failure = e.what(); }
        failed.store(true);
    }
}
void Sing::render(float* left, float* right, unsigned count) noexcept WS_REALTIME {
    std::fill_n(left, count, 0); std::fill_n(right, count, 0);
    auto currentEpoch = epoch.load(std::memory_order_acquire);
    if (renderEpoch != currentEpoch) { renderEpoch = currentEpoch; primed = false; stretch.reset(); }
    auto pop = [&](SingFrame& frame) {
        while (ready.pop(frame)) if (frame.epoch == renderEpoch) return true;
        return false;
    };
    if (!primed) {
        if (ready.size() < targetFrames || !pop(previous) || ready.size() < targetFrames - 1 || !pop(next)) { outputReady.store(false); return; }
        primed = true; fraction = 0; fade = 0;
    }
    bool shifted = std::abs(pitch.load()) > .001f;
    if (shifted != transposing) { stretch.reset(); transposing = shifted; fade = 0; }
    if (shifted) stretch.setTransposeSemitones(std::clamp(pitch.load(), -6.f, 6.f));
    outputReady.store(true);
    for (unsigned offset = 0; offset < count;) {
        unsigned n = std::min(256u, count - offset);
        // Correct slow clock drift without adding a pitch-shifter delay at zero transpose.
        double ratio = 1 + std::clamp((double(ready.size()) - targetFrames) / (rate * 20.), -.002, .002);
        for (unsigned i = 0; i < n; ++i) {
            smoothRemoval += .01f * (std::clamp(removal.load(), 0.f, 1.f) - smoothRemoval);
            float a = float(fraction), b = 1 - a;
            fade = std::min(1.f, fade + 1.f / 128);
            mixed[0][i] = primed ? ((previous.l - smoothRemoval * previous.vl) * b + (next.l - smoothRemoval * next.vl) * a) * fade : 0;
            mixed[1][i] = primed ? ((previous.r - smoothRemoval * previous.vr) * b + (next.r - smoothRemoval * next.vr) * a) * fade : 0;
            if (!primed) continue;
            consumed.store(previous.position, std::memory_order_relaxed);
            fraction += ratio;
            while (fraction >= 1) {
                fraction -= 1; previous = next;
                if (!pop(next)) { primed = false; outputReady.store(false); underruns.fetch_add(1); break; }
            }
        }
        if (shifted) {
            float* ins[]{mixed[0].data(), mixed[1].data()}; float* outs[]{left + offset, right + offset};
            stretch.process(ins, n, outs, n);
        } else {
            std::copy_n(mixed[0].data(), n, left + offset); std::copy_n(mixed[1].data(), n, right + offset);
        }
        for (unsigned i = offset; i < offset + n; ++i) {
            float peak = std::max(std::abs(left[i]), std::abs(right[i]));
            float target = peak > .98f ? .98f / peak : 1;
            gain = target < gain ? target : gain + .0002f * (target - gain);
            left[i] = std::isfinite(left[i]) ? left[i] * gain : 0;
            right[i] = std::isfinite(right[i]) ? right[i] * gain : 0;
        }
        offset += n;
    }
    queued.store(unsigned(ready.size()));
}
}
