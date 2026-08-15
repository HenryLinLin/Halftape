#include "wscore/onnx.hpp"
#include <onnxruntime_cxx_api.h>
#include <coreml_provider_factory.h>
#include <thread>
#include <cstdlib>
namespace ws {
struct OnnxSeparator::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "Halftape"};
    std::unique_ptr<Ort::Session> session;
    int stems;
    bool coreml = false;
};
OnnxSeparator::OnnxSeparator(const std::string& path, int stems) : impl(std::make_unique<Impl>()) {
    impl->stems = stems;
    auto options = [] {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(4); opts.SetInterOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        opts.AddConfigEntry("session.intra_op.allow_spinning", "0");
        return opts;
    };
    auto opts = options();
    if (std::getenv("HALFTAPE_ONNX_COREML") && !std::getenv("HALFTAPE_ONNX_CPU")) {
        try {
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_CoreML(opts, COREML_FLAG_ONLY_ALLOW_STATIC_INPUT_SHAPES));
            impl->session = std::make_unique<Ort::Session>(impl->env, path.c_str(), opts);
            impl->coreml = true;
        } catch (const Ort::Exception&) { impl->session.reset(); }
    }
    if (!impl->session) { auto cpu = options(); impl->session = std::make_unique<Ort::Session>(impl->env, path.c_str(), cpu); }
    auto in = impl->session->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    auto out = impl->session->GetOutputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (in != std::vector<int64_t>{1, 2, 343980} || out != std::vector<int64_t>{1, stems, 2, 343980})
        throw std::runtime_error("Unsupported ONNX model shape");
}
OnnxSeparator::~OnnxSeparator() = default;
const char* OnnxSeparator::backend() const { return impl->coreml ? "ONNX Runtime / CoreML + CPU" : "ONNX Runtime / CPU"; }
Stems OnnxSeparator::separate(const Stereo& audio, const std::function<void(float)>& progress) {
    constexpr size_t length = 343980, stride = length * 3 / 4;
    auto n = audio[0].size();
    Stems result;
    for (auto& s : result) for (auto& c : s) c.assign(n, 0);
    std::vector<float> input(2 * length), weights(n, 0);
    std::array<int64_t, 3> shape{1, 2, int64_t(length)};
    const char* ins[]{"mix"}; const char* outs[]{"stems"};
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    int map[]{1, 2, 5, 0, 3, 4};
    for (size_t start = 0; start < n; start += stride) {
        if (progress) progress(float(start) / n);
        size_t count = std::min(length, n - start);
        std::fill(input.begin(), input.end(), 0);
        for (int c = 0; c < 2; ++c) std::copy_n(audio[c].data() + start, count, input.data() + c * length);
        auto tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
        Ort::RunOptions run;
        std::atomic<bool> cancelled{false}, finished{false};
        std::thread monitor([&] {
            while (!finished.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                if (progress) try { progress(float(start) / n); } catch (...) { cancelled = true; try { run.SetTerminate(); } catch (...) {} return; }
            }
        });
        struct Join { std::atomic<bool>& done; std::thread& thread; ~Join() { done = true; if (thread.joinable()) thread.join(); } } join{finished, monitor};
        auto output = impl->session->Run(run, ins, &tensor, 1, outs, 1);
        finished = true; monitor.join();
        if (cancelled) throw std::runtime_error("cancelled");
        auto* samples = output[0].GetTensorData<float>();
        for (size_t i = 0; i < count; ++i) {
            float weight = float(std::min(i + 1, length - i)) / (length / 2);
            weights[start + i] += weight;
            for (int s = 0; s < impl->stems; ++s) for (int c = 0; c < 2; ++c) {
                float value = samples[(s * 2 + c) * length + i];
                if (!std::isfinite(value)) throw std::runtime_error("Model returned invalid audio");
                result[map[s]][c][start + i] += value * weight;
            }
        }
        if (start + count == n) break;
    }
    for (auto& stem : result) for (auto& ch : stem) for (size_t i = 0; i < n; ++i) ch[i] /= std::max(1e-8f, weights[i]);
    if (progress) progress(1);
    return result;
}
}
