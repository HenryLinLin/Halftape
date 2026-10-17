#include "wscore/wav.hpp"
#include "wscore/separation.hpp"
#include "wscore/resample.hpp"
#include "wscore/streaming.hpp"
#include "wscore/sing.hpp"
#include <chrono>
#include <sys/resource.h>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <pthread.h>
#include <string>
#include <thread>
#include <vector>

namespace {
void help() {
    std::cout << "Halftape 0.1.0\n"
                 "Usage:\n"
                 "  wsbench --version\n"
                 "  wsbench inspect file.wav\n"
                 "  wsbench separate model.bin 4|6 file.wav\n"
                 "  wsbench stream model.onnx mixture.wav [vocals.wav accompaniment.wav] [lookahead]\n"
                 "  wsbench live model.onnx mixture.wav [seconds]\n"
                 "  wsbench sdr reference.wav estimate.wav\n";
}

ws::Stereo readAtRate(const std::string& path) {
    auto wav = ws::readWav(path);
    return ws::resample(wav.audio, wav.sampleRate);
}

double sdrDb(const ws::Stereo& reference, const ws::Stereo& estimate, size_t frames) {
    double signal = 0, error = 0;
    for (int ch = 0; ch < 2; ++ch) for (size_t i = 0; i < frames; ++i) {
        const double r = reference[ch][i], e = r - estimate[ch][i];
        signal += r * r;
        error += e * e;
    }
    if (error == 0) return signal == 0 ? 0 : std::numeric_limits<double>::infinity();
    if (signal == 0) return -std::numeric_limits<double>::infinity();
    return 10.0 * std::log10(signal / error);
}

int stream(const std::string& modelPath, const std::string& mixturePath,
           const std::string* vocalsPath, const std::string* accompanimentPath, unsigned lookahead) {
    auto mixture = readAtRate(mixturePath);
    const size_t frames = mixture[0].size();
    if (!frames) throw std::runtime_error("empty audio");

    const auto loadStart = std::chrono::steady_clock::now();
    auto separator = ws::openLiveSeparator(modelPath, lookahead);
    const double loadSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - loadStart).count();

    const size_t hops = (frames + ws::liveHop - 1) / ws::liveHop;
    std::array<ws::Stereo, 4> stems;
    for (auto& stem : stems) for (auto& channel : stem) channel.reserve(hops * ws::liveHop);
    std::vector<double> hopMs;
    hopMs.reserve(hops + 1);
    auto append = [&](const ws::LiveStems& live) {
        for (size_t s = 0; s < 4; ++s) for (size_t i = 0; i < ws::liveHop; ++i) {
            stems[s][0].push_back(live[s][i].l);
            stems[s][1].push_back(live[s][i].r);
        }
    };

    ws::LiveStems output{};
    auto start = std::chrono::steady_clock::now();
    for (size_t h = 0; h < hops; ++h) {
        ws::LiveBlock input{};
        for (size_t i = 0; i < ws::liveHop; ++i) {
            const size_t source = h * ws::liveHop + i;
            if (source < frames) input[i] = {mixture[0][source], mixture[1][source]};
        }
        const auto hopStart = std::chrono::steady_clock::now();
        if (separator->process(input, output)) append(output);
        hopMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - hopStart).count());
    }
    ws::LiveBlock zero{};
    for (unsigned flushed = 0; flushed + 2 * ws::liveHop <= separator->latency(); flushed += ws::liveHop) {
        const auto flushStart = std::chrono::steady_clock::now();
        if (separator->process(zero, output)) append(output);
        hopMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - flushStart).count());
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // Discard the pre-roll result, then trim the flush-padded output to the source length.
    for (auto& stem : stems) for (auto& channel : stem) {
        if (channel.size() < frames) throw std::runtime_error("streaming separator returned too few frames");
        channel.resize(frames);
    }
    std::sort(hopMs.begin(), hopMs.end());
    const double meanMs = std::accumulate(hopMs.begin(), hopMs.end(), 0.0) / hopMs.size();
    const double p99Ms = hopMs[static_cast<size_t>(std::ceil(hopMs.size() * 0.99)) - 1];
    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
    std::cout << std::fixed << std::setprecision(6)
              << "model_load_s: " << loadSeconds
              << "\nelapsed_s: " << elapsed
              << "\nrtf: " << elapsed / (double(frames) / ws::rate)
              << "\nhop_mean_ms: " << meanMs
              << "\nhop_p99_ms: " << p99Ms
              << "\nhop_max_ms: " << hopMs.back()
              << "\npeak_rss_bytes: " << usage.ru_maxrss << '\n';

    if (vocalsPath && accompanimentPath) {
        auto vocals = readAtRate(*vocalsPath);
        auto accompaniment = readAtRate(*accompanimentPath);
        const size_t count = std::min({frames, vocals[0].size(), accompaniment[0].size()});
        if (!count) throw std::runtime_error("empty reference audio");
        ws::Stereo instrument = mixture;
        for (int ch = 0; ch < 2; ++ch) for (size_t i = 0; i < count; ++i)
            instrument[ch][i] = stems[0][ch][i] + stems[1][ch][i] + stems[3][ch][i];
        std::cout << "quality_frames: " << count
                  << "\nmixture_vs_accompaniment_sdr_db: " << sdrDb(accompaniment, mixture, count)
                  << "\ninstrument_vs_accompaniment_sdr_db: " << sdrDb(accompaniment, instrument, count)
                  << "\nvocals_vs_reference_sdr_db: " << sdrDb(vocals, stems[2], count) << '\n';

        // Diagnostic least-squares fit: instrument ≈ leakage*vocals + gain*accompaniment.
        double vv = 0, aa = 0, va = 0, vi = 0, ai = 0;
        for (int ch = 0; ch < 2; ++ch) for (size_t i = 0; i < count; ++i) {
            const double v = vocals[ch][i], a = accompaniment[ch][i], y = instrument[ch][i];
            vv += v * v; aa += a * a; va += v * a; vi += v * y; ai += a * y;
        }
        const double determinant = vv * aa - va * va;
        if (std::abs(determinant) > 1e-30) {
            std::cout << "instrument_vocal_leakage_coefficient: " << (vi * aa - ai * va) / determinant
                      << "\ninstrument_accompaniment_gain: " << (ai * vv - vi * va) / determinant << '\n';
        } else {
            std::cout << "instrument_vocal_leakage_coefficient: undefined\ninstrument_accompaniment_gain: undefined\n";
        }
        std::cout << "mix_fit_note: 2x2 least-squares diagnostic; not a standalone quality score\n";
    }
    return 0;
}

int live(const std::string& modelPath, const std::string& mixturePath, double seconds) {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    auto wav = ws::readWav(mixturePath);
    if (wav.audio[0].empty()) throw std::runtime_error("empty audio");
    if (!std::isfinite(seconds) || seconds <= 0 || seconds > 3600)
        throw std::runtime_error("seconds must be between 0 and 3600");
    ws::Sing sing(wav.sampleRate, modelPath);
    constexpr uint64_t outputChunk = 256;
    const uint64_t totalOutputFrames = static_cast<uint64_t>(std::ceil(seconds * ws::rate));
    const auto start = std::chrono::steady_clock::now();
    uint64_t outputFrame = 0;
    bool sawNonzero = false;
    double firstNonzeroMs = 0, energy = 0;
    uint64_t sampleCount = 0;
    double delaySum = 0, delayMax = 0;
    uint64_t delayCount = 0;
    double maxWakeLatenessMs = 0;
    uint64_t wakeLateOverPeriod = 0;
    std::array<float, outputChunk> left{}, right{};
    const double periodMs = double(outputChunk) * 1000.0 / ws::rate;

    while (outputFrame < totalOutputFrames) {
        const uint64_t nextFrame = std::min(totalOutputFrames, outputFrame + outputChunk);
        // Derive source counts from the absolute output position to avoid drift for rates such as 48 kHz.
        const uint64_t sourceBegin = outputFrame * wav.sampleRate / ws::rate;
        const uint64_t sourceEnd = nextFrame * wav.sampleRate / ws::rate;
        for (uint64_t i = sourceBegin; i < sourceEnd; ++i) {
            const size_t source = static_cast<size_t>(i % wav.audio[0].size());
            sing.feed({wav.audio[0][source], wav.audio[1][source]});
        }

        const auto deadline = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(double(nextFrame) / ws::rate));
        std::this_thread::sleep_until(deadline);
        const double wakeLatenessMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - deadline).count();
        maxWakeLatenessMs = std::max(maxWakeLatenessMs, wakeLatenessMs);
        if (wakeLatenessMs > periodMs) ++wakeLateOverPeriod;
        const unsigned renderCount = static_cast<unsigned>(nextFrame - outputFrame);
        sing.render(left.data(), right.data(), renderCount);

        const int state = sing.state();
        if (state == 5) throw std::runtime_error("live worker failed: " + sing.error());
        double chunkEnergy = 0;
        bool chunkAudible = false;
        for (unsigned i = 0; i < renderCount; ++i) {
            chunkEnergy += double(left[i]) * left[i] + double(right[i]) * right[i];
            chunkAudible = chunkAudible || std::max(std::abs(left[i]), std::abs(right[i])) > 1e-7f;
        }
        energy += chunkEnergy;
        sampleCount += renderCount * 2;
        if (chunkAudible && !sawNonzero) {
            sawNonzero = true;
            firstNonzeroMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
        }
        if (sawNonzero) {
            const double delay = sing.delay();
            delaySum += delay;
            delayMax = std::max(delayMax, delay);
            ++delayCount;
        }
        outputFrame = nextFrame;
    }

    rusage usage{}; getrusage(RUSAGE_SELF, &usage);
    const double actualSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << std::fixed << std::setprecision(6)
              << "requested_seconds: " << seconds
              << "\nactual_seconds: " << actualSeconds
              << "\noutput_rms: " << std::sqrt(energy / std::max<uint64_t>(1, sampleCount))
              << "\nfirst_nonzero_after_start_ms: ";
    if (sawNonzero) std::cout << firstNonzeroMs << '\n'; else std::cout << "none\n";
    std::cout << "software_queue_delay_mean_s: ";
    if (delayCount) std::cout << delaySum / delayCount << '\n'; else std::cout << "undefined\n";
    std::cout << "software_queue_delay_max_s: ";
    if (delayCount) std::cout << delayMax << '\n'; else std::cout << "undefined\n";
    std::cout << "max_wake_lateness_ms: " << maxWakeLatenessMs
              << "\nwake_lateness_over_period_count: " << wakeLateOverPeriod << '\n';
    std::cout << "underruns: " << sing.underruns.load()
              << "\ndropped_frames: " << sing.dropped.load()
              << "\nworker_rtf: " << sing.rtf.load()
              << "\npeak_rss_bytes: " << usage.ru_maxrss << '\n';
    return 0;
}

int inspect(const std::string& path) {
    auto wav = ws::readWav(path);
    double peak = 0;
    for (const auto& channel : wav.audio)
        for (float sample : channel) peak = std::max(peak, std::abs(double(sample)));
    std::cout << "frames: " << wav.audio[0].size() << '\n'
              << "sample_rate: " << wav.sampleRate << " Hz\n"
              << "duration: " << std::fixed << std::setprecision(6)
              << double(wav.audio[0].size()) / wav.sampleRate << " s\n"
              << "peak: " << std::setprecision(9) << peak << '\n';
    return 0;
}

int sdr(const std::string& referencePath, const std::string& estimatePath) {
    auto reference = ws::readWav(referencePath);
    auto estimate = ws::readWav(estimatePath);
    if (reference.sampleRate != estimate.sampleRate)
        throw std::runtime_error("sample rates do not match");
    if (reference.audio[0].size() != estimate.audio[0].size())
        throw std::runtime_error("frame counts do not match");
    std::cout << std::fixed << std::setprecision(6);
    for (size_t ch = 0; ch < 2; ++ch) {
        double signalPower = 0, errorPower = 0;
        for (size_t i = 0; i < reference.audio[ch].size(); ++i) {
            double signal = reference.audio[ch][i];
            double error = signal - estimate.audio[ch][i];
            signalPower += signal * signal;
            errorPower += error * error;
        }
        std::cout << "channel_" << (ch + 1) << "_sdr_db: ";
        if (signalPower == 0 && errorPower == 0) std::cout << "undefined\n";
        else if (errorPower == 0) std::cout << "inf\n";
        else if (signalPower == 0) std::cout << "-inf\n";
        else std::cout << 10.0 * std::log10(signalPower / errorPower) << '\n';
    }
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            std::cout << "Halftape 0.1.0\n";
            return 0;
        }
        if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
            help();
            return 0;
        }
        if (argc == 5 && std::string(argv[1]) == "separate") {
            auto wav = ws::readWav(argv[4]);
            auto audio = ws::resample(wav.audio, wav.sampleRate);
            if (audio[0].empty()) throw std::runtime_error("empty audio");
            ws::Separator separator;
            separator.load(argv[2], std::stoi(argv[3]));
            auto start = std::chrono::steady_clock::now();
            auto result = separator.separate(audio);
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            rusage usage{}; getrusage(RUSAGE_SELF, &usage);
            double error = 0, power = 0;
            for (size_t i = 0; i < audio[0].size(); ++i) for (int c = 0; c < 2; ++c) {
                double sum = 0; for (auto& stem : result) sum += stem[c][i];
                error += std::pow(sum - audio[c][i], 2); power += std::pow(audio[c][i], 2);
            }
            std::cout << "backend: " << separator.backend() << "\nseconds: " << seconds
                      << "\nrtf: " << seconds / (double(audio[0].size()) / ws::rate)
                      << "\npeak_rss_bytes: " << usage.ru_maxrss
                      << "\nreconstruction_sdr_db: " << 10 * std::log10((power + 1e-30) / (error + 1e-30)) << '\n';
            return 0;
        }
        if (argc >= 4 && argc <= 7 && std::string(argv[1]) == "stream") {
            const bool hasLookahead = argc == 5 || argc == 7;
            const unsigned lookahead = hasLookahead ? unsigned(std::stoul(argv[argc - 1])) : 2;
            if (argc <= 5) return stream(argv[2], argv[3], nullptr, nullptr, lookahead);
            const std::string vocalsPath = argv[4], accompanimentPath = argv[5];
            return stream(argv[2], argv[3], &vocalsPath, &accompanimentPath, lookahead);
        }
        if ((argc == 4 || argc == 5) && std::string(argv[1]) == "live") {
            const double seconds = argc == 5 ? std::stod(argv[4]) : 10.0;
            return live(argv[2], argv[3], seconds);
        }
        if (argc == 3 && std::string(argv[1]) == "inspect") return inspect(argv[2]);
        if (argc == 4 && std::string(argv[1]) == "sdr") return sdr(argv[2], argv[3]);
        help();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
