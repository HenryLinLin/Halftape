#pragma once
#include "audio.hpp"
#include <chrono>
#include <functional>
#include <string>
namespace ws {
using Stems = std::array<Stereo, stemCount>;
class Separator {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    Separator();
    ~Separator();
    void load(const std::string& path, int stems);
    void unload();
    bool loaded() const;
    const char* backend() const;
    Stems separate(const Stereo& audio, const std::function<void(float)>& progress = {});
};
constexpr unsigned segmentFrames = 6 * rate;
struct StemBlock { Stems audio; };
struct StemBank {
    size_t size;
    std::vector<std::unique_ptr<StemBlock>> owned;
    std::unique_ptr<std::atomic<StemBlock*>[]> blocks;
    std::atomic<unsigned> ready{0};
    explicit StemBank(size_t frames) : size((frames + segmentFrames - 1) / segmentFrames), owned(size), blocks(std::make_unique<std::atomic<StemBlock*>[]>(size)) {
        for (size_t i = 0; i < size; ++i) blocks[i].store(nullptr);
    }
    void publish(size_t index, std::unique_ptr<StemBlock> block) {
        auto* ptr = block.get(); owned[index] = std::move(block);
        blocks[index].store(ptr, std::memory_order_release); ready.fetch_add(1);
    }
};
std::vector<size_t> segmentOrder(size_t frames, double loopStart, double loopEnd);
void separateClip(Separator&, const Clip&, StemBank&, double loopStart, double loopEnd,
                  const std::function<bool()>& cancelled, const std::function<void(float)>& progress);
}
