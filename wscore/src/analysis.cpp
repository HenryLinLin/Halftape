#include "wscore/analysis.hpp"
#include <Accelerate/Accelerate.h>
#include <numeric>
namespace ws {
void analyze(Clip& clip) {
    constexpr int window = 4096, hop = 512, bins = window / 2;
    if (clip.audio[0].size() < window) return;
    auto fft = vDSP_create_fftsetup(12, kFFTRadix2);
    if (!fft) throw std::bad_alloc();
    std::array<float, bins> real{}, imag{}, previous{}, magnitudes{};
    std::array<float, window> hann{};
    for (int i = 0; i < window; ++i) hann[i] = .5f - .5f * std::cos(6.283185307 * i / (window - 1));
    DSPSplitComplex split{real.data(), imag.data()};
    std::vector<double> onset;
    std::array<double, 12> chroma{};
    for (size_t start = 0; start + window <= clip.audio[0].size(); start += hop) {
        for (int i = 0; i < bins; ++i) {
            size_t x = start + i * 2;
            real[i] = (clip.audio[0][x] + clip.audio[1][x]) * .5f * hann[i * 2];
            imag[i] = (clip.audio[0][x + 1] + clip.audio[1][x + 1]) * .5f * hann[i * 2 + 1];
        }
        vDSP_fft_zrip(fft, &split, 1, 12, FFT_FORWARD);
        double flux = 0;
        for (int k = 1; k < bins; ++k) {
            float magnitude = std::sqrt(real[k] * real[k] + imag[k] * imag[k]);
            float compressed = std::log1p(magnitude);
            flux += std::max(0.f, compressed - previous[k]); previous[k] = compressed;
            magnitudes[k] = magnitude;
        }
        float spectralPeak = *std::max_element(magnitudes.begin() + 1, magnitudes.end());
        for (int k = 2; k < bins - 1; ++k) {
            if (magnitudes[k] < spectralPeak * .03f || magnitudes[k] <= magnitudes[k-1] || magnitudes[k] <= magnitudes[k+1]) continue;
            double l = std::log(magnitudes[k-1] + 1e-12), m = std::log(magnitudes[k] + 1e-12), r = std::log(magnitudes[k+1] + 1e-12);
            double offset = .5 * (l-r) / (l - 2*m + r);
            double frequency = (k + std::clamp(offset, -.5, .5)) * rate / window;
            if (frequency > 65 && frequency < 2100) {
                double note = 69 + 12 * std::log2(frequency / 440.);
                int nearest = int(std::round(note));
                double weight = std::max(0., 1 - std::abs(note - nearest));
                chroma[(nearest % 12 + 12) % 12] += magnitudes[k] * weight;
            }
        }
        onset.push_back(flux);
    }
    vDSP_destroy_fftsetup(fft);
    double total = std::accumulate(chroma.begin(), chroma.end(), 0.);
    if (total < .001) return;
    const double major[]{6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88};
    const double minor[]{6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17};
    double bestKey = -1;
    for (int mode = 0; mode < 2; ++mode) for (int root = 0; root < 12; ++root) {
        auto* profile = mode ? minor : major;
        double pm = std::accumulate(profile, profile + 12, 0.) / 12;
        double dot = 0, xx = 0, yy = 0;
        for (int k = 0; k < 12; ++k) {
            double x = chroma[(root + k) % 12] - total / 12, y = profile[k] - pm;
            dot += x * y; xx += x * x; yy += y * y;
        }
        double correlation = dot / std::sqrt(xx * yy + 1e-20);
        if (correlation > bestKey) { bestKey = correlation; clip.key = root + mode * 12; }
    }
    if (onset.size() < 100) return;
    std::vector<double> novelty(onset.size());
    for (size_t i = 0; i < onset.size(); ++i) {
        auto lo = i > 8 ? i - 8 : 0, hi = std::min(onset.size(), i + 9);
        double mean = std::accumulate(onset.begin() + lo, onset.begin() + hi, 0.) / (hi - lo);
        novelty[i] = std::max(0., onset[i] - mean);
    }
    double peak = *std::max_element(novelty.begin(), novelty.end());
    if (peak < 1e-6) return;
    for (auto& x : novelty) x /= peak;
    double fps = double(rate) / hop, best = 0; int period = 0;
    for (int lag = int(fps * 60 / 190); lag <= int(fps * 60 / 60); ++lag) {
        double corr = 0;
        for (size_t i = lag; i < novelty.size(); ++i) corr += novelty[i] * novelty[i - lag];
        corr /= novelty.size() - lag;
        double bpm = fps * 60 / lag;
        corr *= std::exp(-.5 * std::pow(std::log(bpm / 120.) / .45, 2));
        if (corr > best) { best = corr; period = lag; }
    }
    if (!period || best < .0001) return;
    clip.bpm = fps * 60 / period;
    std::vector<double> score(novelty.size()); std::vector<int> back(novelty.size(), -1);
    for (int i = 0; i < int(novelty.size()); ++i) {
        score[i] = novelty[i];
        for (int gap = period / 2; gap <= 2 * period && gap <= i; ++gap) {
            double candidate = score[i - gap] + novelty[i] - 4 * std::pow(std::log(double(gap) / period), 2);
            if (candidate > score[i]) { score[i] = candidate; back[i] = i - gap; }
        }
    }
    int at = int(score.size()) - 1;
    for (int i = std::max(0, at - 2 * period); i < int(score.size()); ++i) if (score[i] > score[at]) at = i;
    std::vector<int> positions;
    while (at >= 0) { positions.push_back(at); at = back[at]; }
    std::reverse(positions.begin(), positions.end());
    // TODO: estimate meters other than four.
    std::array<double, 4> accents{};
    for (size_t i = 0; i < positions.size(); ++i) accents[i % 4] += onset[positions[i]];
    clip.downbeat = int(std::max_element(accents.begin(), accents.end()) - accents.begin());
    for (auto p : positions) clip.beats.push_back((double(p * hop) + window * .5) / rate);
}
}
