#pragma once
#include "audio.hpp"
#include <complex>
#include <string>

namespace ws {
constexpr unsigned liveHop = 128;
using LiveBlock = std::array<Frame, liveHop>;
using LiveStems = std::array<LiveBlock, 4>;

class LiveSeparator {
public:
    virtual ~LiveSeparator() = default;
    virtual void reset() = 0;
    virtual bool process(const LiveBlock& input, LiveStems& output) = 0;
    virtual unsigned latency() const { return 2 * liveHop; }
};
class StreamingSeparator final : public LiveSeparator {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit StreamingSeparator(const std::string& path);
    ~StreamingSeparator();
    void reset() override;
    bool process(const LiveBlock& input, LiveStems& output) override;
};

// Masks the mix with a spectrogram model that sees a short window ending a few frames ahead.
class MaskSeparator : public LiveSeparator {
public:
    static constexpr unsigned size = 4096, hop = 1024, bins = size / 2 + 1, lookahead = 2, step = 2;
    static constexpr unsigned delay = 3 * hop + (lookahead + step) * hop - liveHop;
    explicit MaskSeparator(unsigned frames);
    void reset() override;
    bool process(const LiveBlock& input, LiveStems& output) override;
    unsigned latency() const override { return delay + liveHop; }
    unsigned frames() const { return context; }
    float suppression = 1.5f;
    uint64_t starved = 0;
protected:
    // Both buffers are laid out as [channel][bin][frame], oldest frame first.
    virtual void estimate(const std::vector<float>& mixture, std::vector<float>& vocals) = 0;
private:
    using Complex = std::complex<float>;
    unsigned context;
    std::vector<float> window, magnitudes, vocalMagnitudes;
    std::vector<Complex> buffer, twiddle;
    std::vector<uint16_t> reversal;
    std::array<std::vector<float>, 2> history;
    std::vector<std::array<std::vector<Complex>, 2>> spectra;
    std::array<std::array<std::vector<float>, 2>, 2> overlap;
    std::vector<Frame> accompanimentQueue, vocalQueue;
    size_t readAt = 0, writeAt = 0, queued = 0;
    unsigned pending = 0;
    int64_t frame = -2;
    uint64_t received = 0;
    void transform() noexcept;
    void analyze();
    void finish(int64_t index, unsigned column);
    void push(Frame accompaniment, Frame vocals);
};
class UmxSeparator final : public MaskSeparator {
    struct Impl;
    std::unique_ptr<Impl> impl;
    explicit UmxSeparator(std::unique_ptr<Impl> loaded);
    void estimate(const std::vector<float>& mixture, std::vector<float>& vocals) override;
public:
    explicit UmxSeparator(const std::string& path);
    ~UmxSeparator();
};
std::unique_ptr<LiveSeparator> openLiveSeparator(const std::string& path);
}
