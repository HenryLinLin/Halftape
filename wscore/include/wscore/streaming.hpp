#pragma once
#include "audio.hpp"
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
}
