#include "wscore/streaming.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <fstream>
#include <CommonCrypto/CommonDigest.h>
#include <iomanip>
#include <sstream>

static void parity(ws::StreamingSeparator& separator, const char* path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(bytes.data(), CC_LONG(bytes.size()), digest);
    std::ostringstream hash;
    for (auto c : digest) hash << std::hex << std::setfill('0') << std::setw(2) << unsigned(c);
    if (hash.str() != "02d7934fb50934c10e33be67ad53d117904bc025365fcb40f464d1d6476a027d")
        throw std::runtime_error("independent streaming fixture checksum mismatch");
    file.clear(); file.seekg(8);
    auto read = [&](void* data, size_t size) {
        if (!file.read(static_cast<char*>(data), size)) throw std::runtime_error("truncated parity fixture");
    };
    uint32_t cases; read(&cases, 4);
    float maxError = 0;
    for (unsigned test = 0; test < cases; ++test) {
        uint32_t frames; read(&frames, 4);
        std::vector<float> input(2 * frames), expected(8 * frames);
        read(input.data(), input.size() * sizeof(float)); read(expected.data(), expected.size() * sizeof(float));
        separator.reset();
        unsigned hops = (frames + ws::liveHop - 1) / ws::liveHop;
        for (unsigned hop = 0; hop <= hops; ++hop) {
            ws::LiveBlock block{}; ws::LiveStems stems{};
            for (unsigned i = 0; i < ws::liveHop && hop * ws::liveHop + i < frames; ++i)
                block[i] = {input[hop * ws::liveHop + i], input[frames + hop * ws::liveHop + i]};
            bool valid = separator.process(block, stems);
            if (valid != (hop != 0)) throw std::runtime_error("incorrect reference pre-roll");
            if (!valid) continue;
            for (unsigned s = 0; s < 4; ++s) for (unsigned i = 0; i < ws::liveHop; ++i) {
                auto at = (hop - 1) * ws::liveHop + i;
                if (at >= frames) break;
                maxError = std::max({maxError, std::abs(stems[s][i].l - expected[2 * s * frames + at]),
                    std::abs(stems[s][i].r - expected[(2 * s + 1) * frames + at])});
            }
        }
    }
    std::cout << "reference_cases=" << cases << " max_error=" << maxError << '\n';
    if (maxError > 1e-5f) throw std::runtime_error("streaming stems differ from the independent reference");
}

int main(int argc, char** argv) {
    if (argc < 2 || !std::filesystem::exists(argv[1])) { std::cout << "streaming model not installed\n"; return 77; }
    try {
        auto start = std::chrono::steady_clock::now();
        ws::StreamingSeparator separator(argv[1]);
        double load = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        ws::LiveBlock input{}, previous{};
        ws::LiveStems stems{}, first{};
        std::vector<double> times;
        double error = 0, signal = 0;
        for (unsigned hop = 0; hop < 1600; ++hop) {
            for (unsigned i = 0; i < ws::liveHop; ++i) {
                double t = double(hop * ws::liveHop + i) / ws::rate;
                input[i] = {float(.15 * std::sin(t * 2764.6) + .08 * std::sin(t * 1382.3)), float(.12 * std::sin(t * 3015.9))};
            }
            start = std::chrono::steady_clock::now();
            bool valid = separator.process(input, stems);
            times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            if (valid != (hop > 0)) throw std::runtime_error("incorrect streaming pre-roll");
            if (hop == 1) first = stems;
            if (valid) for (unsigned i = 0; i < ws::liveHop; ++i) {
                float l = 0, r = 0;
                for (auto& s : stems) { l += s[i].l; r += s[i].r; }
                error += std::pow(l - previous[i].l, 2) + std::pow(r - previous[i].r, 2);
                signal += std::pow(previous[i].l, 2) + std::pow(previous[i].r, 2);
            }
            previous = input;
        }
        if (error / signal > 1e-8) throw std::runtime_error("streaming output is misaligned or does not reconstruct the mix");
        separator.reset();
        for (unsigned hop = 0; hop < 2; ++hop) {
            for (unsigned i = 0; i < ws::liveHop; ++i) {
                double t = double(hop * ws::liveHop + i) / ws::rate;
                input[i] = {float(.15 * std::sin(t * 2764.6) + .08 * std::sin(t * 1382.3)), float(.12 * std::sin(t * 3015.9))};
            }
            separator.process(input, stems);
        }
        for (unsigned s = 0; s < 4; ++s) for (unsigned i = 0; i < ws::liveHop; ++i)
            if (std::abs(stems[s][i].l - first[s][i].l) > 1e-6 || std::abs(stems[s][i].r - first[s][i].r) > 1e-6)
                throw std::runtime_error("streaming reset did not clear every recurrent state");
        if (argc > 2) parity(separator, argv[2]);
        double total = std::accumulate(times.begin(), times.end(), 0.);
        std::sort(times.begin(), times.end());
        std::cout << "load_s=" << load << " mean_ms=" << total / times.size() << " p99_ms=" << times[size_t(times.size() * .99)]
                  << " max_ms=" << times.back() << " rtf=" << total / 1000 / (times.size() * double(ws::liveHop) / ws::rate)
                  << " reconstruction_sdr=" << -10 * std::log10(std::max(1e-20, error / signal)) << '\n';
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
