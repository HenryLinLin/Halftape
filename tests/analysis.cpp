#include "wscore/analysis.hpp"
#include "wscore/separation.hpp"
#include <iostream>
#include <chrono>
int main() {
    ws::Clip clip;
    for (auto& c : clip.audio) c.resize(30 * ws::rate);
    for (size_t i = 0; i < clip.audio[0].size(); ++i) {
        double t = double(i) / ws::rate, beat = std::fmod(t, .5);
        float x = .3 * std::exp(-beat * 100) * std::sin(beat * 6.283185307 * 120);
        for (double f : {220., 261.625565, 329.627557}) x += .1 * std::sin(t * 6.283185307 * f);
        clip.audio[0][i] = clip.audio[1][i] = x;
    }
    auto start = std::chrono::steady_clock::now(); ws::analyze(clip);
    auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::cout << "analysis_ms: " << ms << " bpm: " << clip.bpm << " beats: " << clip.beats.size() << " key: " << clip.key << '\n';
    if (std::abs(clip.bpm - 120) > 3 || clip.beats.size() < 50 || clip.key != 21) return 1;
    auto order = ws::segmentOrder(30 * ws::rate, 18, 23);
    if (order.front() != 2 && order.front() != 3) return 2;
    for (size_t i = 1; i < clip.beats.size(); ++i) if (clip.beats[i] <= clip.beats[i-1]) return 3;
    return 0;
}
