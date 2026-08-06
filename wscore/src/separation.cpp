#include "wscore/separation.hpp"
#include "model.hpp"
#ifdef WS_ONNX
#include "wscore/onnx.hpp"
#endif
#include <numeric>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <CommonCrypto/CommonDigest.h>
namespace ws {
struct Separator::Impl {
    std::unique_ptr<demucscpp::demucs_model> model;
#ifdef WS_ONNX
    std::unique_ptr<OnnxSeparator> onnx;
#endif
};
Separator::Separator() : impl(std::make_unique<Impl>()) {}
Separator::~Separator() = default;
bool Separator::loaded() const {
#ifdef WS_ONNX
    if (impl->onnx) return true;
#endif
    return bool(impl->model);
}
const char* Separator::backend() const {
#ifdef WS_ONNX
    if (impl->onnx) return impl->onnx->backend();
#endif
    return "demucs.cpp / Accelerate";
}
void Separator::unload() {
    impl->model.reset();
#ifdef WS_ONNX
    impl->onnx.reset();
#endif
}
void Separator::load(const std::string& path, int stems) {
    if (stems != 4 && stems != 6) throw std::invalid_argument("expected four or six stems");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Model file is not readable");
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    std::array<char, 65536> bytes{};
    while (file.read(bytes.data(), bytes.size()) || file.gcount())
        CC_SHA256_Update(&hash, bytes.data(), CC_LONG(file.gcount()));
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(digest, &hash);
    std::ostringstream hex;
    for (auto c : digest) hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(c);
    bool isOnnx = path.ends_with(".onnx");
    const char* expected = isOnnx ? (stems == 6 ? "7ce55792e2231c93fbf92de95f5fd5b3a5e6c89f7db690dfd693e8f1dce56869" : "d05c269d0178d2a72ad484b10b11dd370193fc923201c3b27a99f848745db70a") : stems == 6 ? "09704f4ceae204e56e77d5eefd6ac71d7275be81fd507e6913371d59abcee856" : "72b17c42d308982ddb5069bc3bf48b81a5aac4cb6516e4366c0fa7cef6df0064";
    if (hex.str() != expected) throw std::runtime_error("Model checksum differs from the supported weights. Use scripts/models.sh to download a compatible model.");
    unload();
    if (isOnnx) {
#ifdef WS_ONNX
        impl->onnx = std::make_unique<OnnxSeparator>(path, stems); return;
#else
        throw std::runtime_error("This build does not include ONNX Runtime");
#endif
    }
    auto model = std::make_unique<demucscpp::demucs_model>();
    if (!demucscpp::load_demucs_model(path, model.get())) throw std::runtime_error("Cannot load Demucs weights");
    if ((model->is_4sources ? 4 : 6) != stems) throw std::runtime_error("Wrong model stem count");
    impl->model = std::move(model);
}
Stems Separator::separate(const Stereo& audio, const std::function<void(float)>& progress) {
#ifdef WS_ONNX
    if (impl->onnx) return impl->onnx->separate(audio, progress);
#endif
    if (!impl->model) throw std::runtime_error("Choose htdemucs weights in Settings to enable stems");
    const auto n = audio[0].size();
    Stems result;
    for (auto& s : result) for (auto& c : s) c.assign(n, 0);
    if (n < 2) return result;
    Eigen::MatrixXf input(2, n);
    double mean = 0, square = 0;
    for (size_t i = 0; i < n; ++i) {
        input(0, i) = audio[0][i]; input(1, i) = audio[1][i];
        double m = (audio[0][i] + audio[1][i]) * .5; mean += m; square += m * m;
    }
    if (square / n - std::pow(mean / n, 2) < 1e-12) { result[5] = audio; return result; }
    auto tensor = demucscpp::demucs_inference(*impl->model, input, [&](float p, const std::string&) { if (progress) progress(p); });
    const int mapping[]{1, 2, 5, 0, 3, 4};
    for (int s = 0; s < (impl->model->is_4sources ? 4 : 6); ++s)
        for (int c = 0; c < 2; ++c) for (size_t i = 0; i < n; ++i) {
            float v = tensor(s, c, i);
            if (!std::isfinite(v)) throw std::runtime_error("Model returned invalid audio");
            result[mapping[s]][c][i] = v;
        }
    return result;
}
std::vector<size_t> segmentOrder(size_t frames, double a, double b) {
    std::vector<size_t> order((frames + segmentFrames - 1) / segmentFrames);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [=](size_t x, size_t y) {
        auto distance = [=](size_t i) { return std::max({0., a * rate - double((i + 1) * segmentFrames), double(i * segmentFrames) - b * rate}); };
        return distance(x) < distance(y);
    });
    return order;
}
void separateClip(Separator& separator, const Clip& clip, StemBank& bank, double a, double b,
                  const std::function<bool()>& cancelled, const std::function<void(float)>& progress) {
    auto total = clip.audio[0].size();
    for (auto index : segmentOrder(total, a, b)) {
        if (cancelled()) return;
        if (bank.blocks[index].load(std::memory_order_acquire)) continue;
        size_t begin = index * segmentFrames, end = std::min(total, begin + segmentFrames);
        constexpr size_t context = rate * 9 / 10;
        size_t lo = begin > context ? begin - context : 0, hi = std::min(total, end + context);
        Stereo input;
        for (int c = 0; c < 2; ++c) input[c].assign(clip.audio[c].begin() + lo, clip.audio[c].begin() + hi);
        auto stems = separator.separate(input, [&](float p) { if (cancelled()) throw std::runtime_error("cancelled"); progress((bank.ready.load() + p) / bank.size); });
        if (cancelled()) return;
        auto block = std::make_unique<StemBlock>();
        block->audio = std::move(stems);
        block->offset = uint32_t(begin - lo);
        bank.publish(index, std::move(block));
        progress(float(bank.ready.load()) / bank.size);
    }
}
}
