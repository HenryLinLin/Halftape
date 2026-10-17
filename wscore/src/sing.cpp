#include "wscore/sing.hpp"
#include "wscore/resample.hpp"
#include <pthread.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

namespace ws {
void realtimePolicy(double periodSeconds, double computationSeconds) noexcept {
#ifdef __APPLE__
    mach_timebase_info_data_t info{};
    if (mach_timebase_info(&info) != KERN_SUCCESS || !info.numer || periodSeconds <= 0) return;
    auto ticks = [&](double seconds) { return uint32_t(seconds * 1e9 * info.denom / info.numer); };
    double computation = std::clamp(computationSeconds, periodSeconds * .05, periodSeconds * .5);
    thread_time_constraint_policy_data_t policy{ticks(periodSeconds), ticks(computation), ticks(periodSeconds), TRUE};
    auto self = mach_thread_self();
    thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    mach_port_deallocate(mach_task_self(), self);
#else
    (void)periodSeconds; (void)computationSeconds;
#endif
}
Sing::Sing(unsigned sampleRate, const std::string& model) : Sing(sampleRate, openLiveSeparator(model)) {}
Sing::Sing(unsigned sampleRate, std::unique_ptr<LiveSeparator> processor)
    : separator(std::move(processor)), sourceRate(sampleRate) {
    if (sampleRate < 8000 || sampleRate > 192000 || !separator) throw std::runtime_error("Unsupported live audio format");
    // Block separators pause the worker for each model run, so they start with one more hop of cushion.
    targetFrames.store(separator->latency() > 4 * liveHop ? 5120 : 4096);
    stretch.presetDefault(2, rate);
    worker = std::thread([this] { run(); });
}
Sing::~Sing() { quit.store(true); wake.release(); if (worker.joinable()) worker.join(); }
void Sing::feed(Frame frame) noexcept {
    auto position = captured.fetch_add(1, std::memory_order_relaxed);
    if ((position & 1023) == 0) lastFeed.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
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
    const double hopSeconds = double(liveHop) / rate;
    try {
        r8b::CDSPResampler leftSRC(sourceRate, rate, 256, 4., 180.15, r8b::fprMinPhase);
        r8b::CDSPResampler rightSRC(sourceRate, rate, 256, 4., 180.15, r8b::fprMinPhase);
        std::array<double, 256> l{}, r{};
        LiveBlock hop{}; LiveStems stems{};
        unsigned fill = 0;
        uint64_t expected = 0, origin = 0, resampled = 0, generation = 0, flushedAt = 0;
        unsigned overruns = 0, windowHops = 0, slowWindows = 0;
        double worst = 0, previousWorst = 0, windowSeconds = 0, ceiling = targetFrames.load();
        uint64_t backlog = 0, previousBacklog = 0;
        bool flushed = false, relaxPending = false;
        auto lastOverrun = std::chrono::steady_clock::time_point{};
        auto relax = [&] {
            if (!separator->relax()) return false;
            relaxed.fetch_add(1); relaxPending = true; overruns = 0; slowWindows = 0; return true;
        };
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
            worst = std::max(worst, elapsed); windowSeconds += elapsed;
            if (++windowHops >= rate / liveHop) {
                // A window that took longer than the audio it covers means the model is falling behind.
                slowWindows = windowSeconds > 1.0 ? slowWindows + 1 : 0;
                if (slowWindows >= 2) relax();
                windowHops = 0; previousWorst = worst; worst = 0; windowSeconds = 0;
                previousBacklog = backlog; backlog = 0;
                ceiling *= .75;
                boost.store(boost.load() * 9 / 10);
                // Ask for at most half of each period so a slow run never trips the scheduler's overrun failsafe.
                double period = hopSeconds * std::ceil(std::max(1., 4 * previousWorst / hopSeconds));
                realtimePolicy(period, std::min(2 * previousWorst, .5 * period));
            }
            // The cushion covers the slowest recent model run, the longest the worker recently waited for input,
            // and one output block; underruns add a decaying boost, and the starting size shrinks toward it.
            double slowest = std::max(worst, previousWorst);
            double lateness = double(std::max(backlog, previousBacklog)) * rate / sourceRate;
            double cushion = slowest * rate * 1.5 + lateness + 512 + liveHop + boost.load();
            auto target = unsigned(std::max(cushion, ceiling));
            targetFrames.store(std::clamp(target, std::min(minimumTarget.load(), 8192u), 8192u));
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
            wake.try_acquire_for(std::chrono::milliseconds(50));
            if (quit.load()) break;
            auto audible = lastAudible.load(), total = captured.load();
            auto sinceFeed = std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(lastFeed.load());
            double stalledSeconds = std::max(.2, 4. * 1024 / sourceRate);
            bool stalled = sinceFeed > std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(stalledSeconds));
            bool silent = total >= audible + uint64_t(std::max(0.f, silenceSeconds.load()) * sourceRate);
            if (flushed && audible != flushedAt) flushed = false;
            if (!flushed && (stalled || silent)) { flushed = true; flushedAt = audible; reset(expected); }
            if (relaxPending) { relaxPending = false; reset(expected); }
            backlog = std::max<uint64_t>(backlog, input.size());
            SingInput frame;
            if (input.size() > sourceRate / 4) {
                auto now = std::chrono::steady_clock::now();
                overruns = now - lastOverrun < std::chrono::seconds(5) ? overruns + 1 : 1;
                lastOverrun = now;
                if (overruns >= 2) relax();
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
    if (renderEpoch != currentEpoch) {
        renderEpoch = currentEpoch;
        if (primed) ramp = 128;
        primed = false; stretch.reset();
        SingFrame stale;
        while (ready.peek(stale) && stale.epoch != renderEpoch) ready.pop(stale);
    }
    auto pop = [&](SingFrame& frame) {
        while (ready.pop(frame)) if (frame.epoch == renderEpoch) return true;
        return false;
    };
    auto target = targetFrames.load(std::memory_order_relaxed);
    if (!primed) {
        if (ready.size() < target || !pop(previous) || ready.size() < target - 1 || !pop(next)) {
            outputReady.store(false);
            for (unsigned i = 0; i < count && ramp; ++i, --ramp) {
                left[i] = lastLeft * ramp / 128; right[i] = lastRight * ramp / 128;
            }
            if (!ramp) lastLeft = lastRight = 0;
            queued.store(unsigned(ready.size()));
            return;
        }
        primed = true; fraction = 0; fade = 0; ramp = 0;
    }
    bool shifted = std::abs(pitch.load()) > .001f;
    if (shifted != transposing) { stretch.reset(); transposing = shifted; fade = 0; }
    if (shifted) stretch.setTransposeSemitones(std::clamp(pitch.load(), -6.f, 6.f));
    outputReady.store(true);
    for (unsigned offset = 0; offset < count;) {
        unsigned n = std::min(256u, count - offset);
        // Correct slow clock drift without adding a pitch-shifter delay at zero transpose.
        double ratio = 1 + std::clamp((double(ready.size()) - target) / (rate * 20.), -.002, .002);
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
                if (!pop(next)) {
                    primed = false; outputReady.store(false); underruns.fetch_add(1);
                    boost.store(std::min(8192u, boost.load() * 5 / 4 + 256));
                    break;
                }
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
            float desired = peak > .98f ? .98f / peak : 1;
            gain = desired < gain ? desired : gain + .0002f * (desired - gain);
            left[i] = std::isfinite(left[i]) ? left[i] * gain : 0;
            right[i] = std::isfinite(right[i]) ? right[i] * gain : 0;
        }
        offset += n;
    }
    if (count) { lastLeft = left[count - 1]; lastRight = right[count - 1]; }
    queued.store(unsigned(ready.size()));
}
}
