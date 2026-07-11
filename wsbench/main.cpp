#include "wscore/wav.hpp"
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
void help() {
    std::cout << "Halftape 0.1.0\n"
                 "Usage:\n"
                 "  wsbench --version\n"
                 "  wsbench inspect file.wav\n"
                 "  wsbench sdr reference.wav estimate.wav\n";
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
        if (argc == 3 && std::string(argv[1]) == "inspect") return inspect(argv[2]);
        if (argc == 4 && std::string(argv[1]) == "sdr") return sdr(argv[2], argv[3]);
        help();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
