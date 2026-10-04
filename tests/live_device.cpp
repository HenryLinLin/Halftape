#include "mactap/platform.hpp"
#include "wscore/sing.hpp"
#include "wscore/wav.hpp"
#include <pthread.h>
#include <iostream>

struct DeviceProbe {
    ws::Sing& sing;
    std::atomic<uint64_t> requested{0};
    uint64_t rendered = 0, measured = 0;
    unsigned maxBlock = 0, events = 0;
    std::array<double, 64> dropoutTimes{};
    double firstAudio = -1, energy = 0, delaySum = 0, delayMax = 0;
    static void render(void* raw, float* left, float* right, unsigned count) noexcept WS_REALTIME {
        auto& p = *static_cast<DeviceProbe*>(raw);
        p.maxBlock = std::max(p.maxBlock, count);
        p.requested.fetch_add(count, std::memory_order_release);
        auto before = p.sing.underruns.load();
        p.sing.render(left, right, count);
        if (p.sing.underruns.load() != before && p.events < p.dropoutTimes.size())
            p.dropoutTimes[p.events++] = double(p.rendered) / ws::rate;
        for (unsigned i = 0; i < count; ++i) {
            p.energy += double(left[i]) * left[i] + double(right[i]) * right[i];
            if (p.firstAudio < 0 && std::max(std::abs(left[i]), std::abs(right[i])) > 1e-7f)
                p.firstAudio = double(p.rendered + i) / ws::rate;
        }
        if (p.firstAudio >= 0) {
            double delay = p.sing.delay();
            p.delaySum += delay; p.delayMax = std::max(p.delayMax, delay); ++p.measured;
        }
        p.rendered += count;
        std::fill_n(left, count, 0); std::fill_n(right, count, 0);
    }
};

int main(int argc, char** argv) {
    if (argc < 3) { std::cerr << "live_device_tests model.onnx mixture.wav [seconds]\n"; return 2; }
    try {
        auto audio = ws::readWav(argv[2]);
        if (audio.audio[0].empty()) throw std::runtime_error("empty audio");
        double seconds = argc > 3 ? std::stod(argv[3]) : 60;
        if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600) throw std::runtime_error("invalid duration");
        ws::Sing sing(audio.sampleRate, argv[1]);
        DeviceProbe probe{sing}; ws::Output output;
        std::atomic<bool> quit{false};
        double maxFeederGap = 0;
        std::thread feeder([&] {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            uint64_t sent = 0;
            auto last = std::chrono::steady_clock::now();
            while (!quit.load()) {
                auto now = std::chrono::steady_clock::now();
                maxFeederGap = std::max(maxFeederGap, std::chrono::duration<double>(now - last).count()); last = now;
                auto until = probe.requested.load(std::memory_order_acquire) * audio.sampleRate / ws::rate;
                while (sent < until) {
                    auto at = sent++ % audio.audio[0].size();
                    sing.feed({audio.audio[0][at], audio.audio[1][at]});
                }
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
        });
        struct Join { std::thread& thread; std::atomic<bool>& quit; ~Join() { quit = true; if (thread.joinable()) thread.join(); } } join{feeder, quit};
        output.start(DeviceProbe::render, &probe, true);
        auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end && sing.state() != 5)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        output.stop();
        quit = true; feeder.join();
        std::cout << "device_clocked_seconds=" << double(probe.rendered) / ws::rate
                  << " first_audio_s=" << probe.firstAudio
                  << " queue_mean_s=" << probe.delaySum / std::max(uint64_t(1), probe.measured)
                  << " queue_max_s=" << probe.delayMax
                  << " output_rms=" << std::sqrt(probe.energy / std::max(uint64_t(1), 2 * probe.rendered))
                  << " underruns=" << sing.underruns.load() << " dropped=" << sing.dropped.load()
                  << " worker_rtf=" << sing.rtf.load() << " max_hop_s=" << sing.maxHopSeconds.load()
                  << " cpu_rtf=" << sing.cpuRtf.load()
                  << " max_block=" << probe.maxBlock << " max_feeder_gap_s=" << maxFeederGap << '\n';
        for (unsigned i = 0; i < probe.events; ++i) std::cout << "underrun_at_s=" << probe.dropoutTimes[i] << '\n';
        if (sing.state() == 5) throw std::runtime_error(sing.error());
        return sing.underruns.load() || sing.dropped.load() || probe.firstAudio < 0 ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
