#include "wscore/streaming.hpp"
#include <chrono>
#include <CommonCrypto/CommonDigest.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <pthread.h>
#ifdef WS_ONNX
#define ACCELERATE_NEW_LAPACK
#include <onnxruntime_cxx_api.h>
#include "live_math.hpp"
#endif

namespace ws {
[[maybe_unused]] static const char* stemgenrt = "77164d6a581fafb2a31f53fd8ffde44c07cf618472952a4cdba14e68dda3b8b9";
[[maybe_unused]] static std::string digest(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Choose a Sing model. Run scripts/sing-clean-model.sh or scripts/sing-model.sh.");
    CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
    std::array<char, 65536> bytes{};
    while (file.read(bytes.data(), bytes.size()) || file.gcount()) CC_SHA256_Update(&hash, bytes.data(), CC_LONG(file.gcount()));
    unsigned char sum[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(sum, &hash);
    std::ostringstream hex;
    for (auto c : sum) hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(c);
    return hex.str();
}
#ifdef WS_ONNX
struct StreamingSeparator::Impl {
    LiveDftOp<float> fft; LiveDftOp<double> fftDouble;
    LiveMatMulOp<float> mat; LiveMatMulOp<double> matDouble;
    Ort::CustomOpDomain math{""};
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "Halftape Live"};
    std::unique_ptr<Ort::Session> session;
    Ort::RunOptions run;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::array<const char*, 9> inputs{"audio_chunk", "audio_history", "fusion_hidden", "spectral_numerator_tail", "waveform_tail", "attention_keys", "attention_values", "spec_memory_hidden", "waveform_memory_hidden"};
    std::array<const char*, 9> outputs{"separated_chunk", "next_audio_history", "next_fusion_hidden", "next_spectral_numerator_tail", "next_waveform_tail", "next_attention_keys", "next_attention_values", "next_spec_memory_hidden", "next_waveform_memory_hidden"};
    std::array<std::vector<int64_t>, 9> shapes{{{1,2,128}, {1,2,896}, {2,1,1000}, {1,4,2,128}, {1,4,2,128}, {1,31,64}, {1,31,128}, {1,1,500}, {1,1,500}}};
    std::array<std::vector<float>, 9> inData, outData;
    std::vector<Ort::Value> inValues, outValues;
    bool pending = false;

    explicit Impl(const std::string& path) {
        if (digest(path) != stemgenrt)
            throw std::runtime_error("Unsupported streaming model. Download the supported model with scripts/sing-model.sh.");
        Ort::SessionOptions options;
        math.Add(&fft); math.Add(&fftDouble); math.Add(&mat); math.Add(&matDouble); options.Add(math);
        options.SetIntraOpNumThreads(1); options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ORT_SEQUENTIAL);
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
        options.AddConfigEntry("session.inter_op.allow_spinning", "0");
        options.AddConfigEntry("mlas.disable_kleidiai", "1");
        session = std::make_unique<Ort::Session>(env, path.c_str(), options);
        if (session->GetInputCount() != 9 || session->GetOutputCount() != 9) throw std::runtime_error("Unsupported streaming state count");
        Ort::AllocatorWithDefaultOptions allocator;
        for (size_t i = 0; i < 9; ++i) {
            auto outShape = i ? shapes[i] : std::vector<int64_t>{1,4,2,128};
            auto inType = session->GetInputTypeInfo(i), outType = session->GetOutputTypeInfo(i);
            if (session->GetInputNameAllocated(i, allocator).get() != std::string(inputs[i]) ||
                session->GetOutputNameAllocated(i, allocator).get() != std::string(outputs[i]) ||
                inType.GetTensorTypeAndShapeInfo().GetShape() != shapes[i] || outType.GetTensorTypeAndShapeInfo().GetShape() != outShape ||
                inType.GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                outType.GetTensorTypeAndShapeInfo().GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
                throw std::runtime_error("Unsupported streaming tensor interface");
            size_t inSize = 1, outSize = 1;
            for (auto n : shapes[i]) inSize *= n;
            for (auto n : outShape) outSize *= n;
            inData[i].resize(inSize); outData[i].resize(outSize);
            inValues.push_back(Ort::Value::CreateTensor<float>(memory, inData[i].data(), inSize, shapes[i].data(), shapes[i].size()));
            outValues.push_back(Ort::Value::CreateTensor<float>(memory, outData[i].data(), outSize, outShape.data(), outShape.size()));
        }
    }
};
StreamingSeparator::StreamingSeparator(const std::string& path) : impl(std::make_unique<Impl>(path)) {}
StreamingSeparator::~StreamingSeparator() = default;
void StreamingSeparator::reset() {
    for (auto& data : impl->inData) std::fill(data.begin(), data.end(), 0);
    impl->pending = false;
}
bool StreamingSeparator::process(const LiveBlock& input, LiveStems& output) {
#if defined(__MAC_15_0)
    if (__builtin_available(macOS 15.0, *)) BLASSetThreading(BLAS_THREADING_SINGLE_THREADED);
#endif
    for (size_t i = 0; i < liveHop; ++i) {
        if (!std::isfinite(input[i].l) || !std::isfinite(input[i].r)) { reset(); throw std::runtime_error("Invalid live input"); }
        impl->inData[0][i] = input[i].l; impl->inData[0][liveHop + i] = input[i].r;
    }
    try {
        impl->session->Run(impl->run, impl->inputs.data(), impl->inValues.data(), 9, impl->outputs.data(), impl->outValues.data(), 9);
        for (size_t i = 0; i < 9; ++i) {
            for (float x : impl->outData[i]) if (!std::isfinite(x)) throw std::runtime_error("Invalid streaming model output");
            if (i) std::copy(impl->outData[i].begin(), impl->outData[i].end(), impl->inData[i].begin());
        }
        for (size_t s = 0; s < 4; ++s) for (size_t i = 0; i < liveHop; ++i)
            output[s][i] = {impl->outData[0][s * 2 * liveHop + i], impl->outData[0][(s * 2 + 1) * liveHop + i]};
        bool valid = impl->pending; impl->pending = true; return valid;
    } catch (...) { reset(); throw; }
}
#ifdef __APPLE__
static OrtCustomThreadHandle interactiveThread(void*, OrtThreadWorkerFn work, void* param) {
    auto* thread = new std::thread([work, param] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        work(param);
    });
    return reinterpret_cast<OrtCustomThreadHandle>(thread);
}
static void joinThread(OrtCustomThreadHandle handle) {
    auto* thread = reinterpret_cast<std::thread*>(const_cast<OrtCustomHandleType*>(handle));
    thread->join(); delete thread;
}
#endif
struct UmxSeparator::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "Halftape Clean Sing"};
    std::unique_ptr<Ort::Session> session;
    Ort::RunOptions run;
    Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<int64_t> shape;
    std::string input, output;
    unsigned frames = 0, step = 1;
    explicit Impl(const std::string& path) {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(2); options.SetInterOpNumThreads(1);
        options.SetExecutionMode(ORT_SEQUENTIAL);
        options.AddConfigEntry("session.intra_op.allow_spinning", "0");
#ifdef __APPLE__
        options.SetCustomCreateThreadFn(interactiveThread); options.SetCustomJoinThreadFn(joinThread);
#endif
        session = std::make_unique<Ort::Session>(env, path.c_str(), options);
        constexpr const char* unsupported = "Unsupported Sing model. Run scripts/sing-model.sh or scripts/sing-clean-model.sh.";
        if (session->GetInputCount() != 1 || session->GetOutputCount() != 1) throw std::runtime_error(unsupported);
        Ort::AllocatorWithDefaultOptions allocator;
        input = session->GetInputNameAllocated(0, allocator).get(); output = session->GetOutputNameAllocated(0, allocator).get();
        auto inType = session->GetInputTypeInfo(0), outType = session->GetOutputTypeInfo(0);
        auto in = inType.GetTensorTypeAndShapeInfo(), out = outType.GetTensorTypeAndShapeInfo();
        shape = in.GetShape();
        if (input != "mag" || shape.size() != 4 || shape[0] != 1 || shape[1] != 2 || shape[2] != MaskSeparator::bins || shape[3] < 4 ||
            out.GetShape() != shape || in.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || out.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            throw std::runtime_error(unsupported);
        frames = unsigned(shape[3]);
        // Warm the session up and pick how often the model runs: every hop when a run fits comfortably inside one.
        std::vector<float> warm(size_t(2) * MaskSeparator::bins * frames), result(warm.size());
        double fastest = 1;
        for (int i = 0; i < 3; ++i) {
            auto begin = std::chrono::steady_clock::now();
            estimate(warm, result);
            fastest = std::min(fastest, std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
        }
        if (fastest > .5 * MaskSeparator::hop / rate) step = 2;
    }
    void estimate(const std::vector<float>& mixture, std::vector<float>& vocals) {
        auto in = Ort::Value::CreateTensor<float>(memory, const_cast<float*>(mixture.data()), mixture.size(), shape.data(), shape.size());
        auto out = Ort::Value::CreateTensor<float>(memory, vocals.data(), vocals.size(), shape.data(), shape.size());
        const char* inputs[]{input.c_str()}; const char* outputs[]{output.c_str()};
        session->Run(run, inputs, &in, 1, outputs, &out, 1);
        for (float& x : vocals) if (!std::isfinite(x)) throw std::runtime_error("Invalid Sing model output");
    }
};
UmxSeparator::UmxSeparator(const std::string& path, unsigned ahead) : UmxSeparator(std::make_unique<Impl>(path), ahead) {}
UmxSeparator::UmxSeparator(std::unique_ptr<Impl> loaded, unsigned ahead) : MaskSeparator(loaded->frames, ahead, loaded->step), impl(std::move(loaded)) {}
UmxSeparator::~UmxSeparator() = default;
void UmxSeparator::estimate(const std::vector<float>& mixture, std::vector<float>& vocals) { impl->estimate(mixture, vocals); }
std::unique_ptr<LiveSeparator> openLiveSeparator(const std::string& path, unsigned lookahead) {
    if (digest(path) == stemgenrt) return std::make_unique<StreamingSeparator>(path);
    return std::make_unique<UmxSeparator>(path, lookahead);
}
#else
struct StreamingSeparator::Impl {};
StreamingSeparator::StreamingSeparator(const std::string&) { throw std::runtime_error("Streaming Sing requires ONNX Runtime."); }
StreamingSeparator::~StreamingSeparator() = default;
void StreamingSeparator::reset() {}
bool StreamingSeparator::process(const LiveBlock&, LiveStems&) { return false; }
struct UmxSeparator::Impl { unsigned frames = 0; };
UmxSeparator::UmxSeparator(const std::string&, unsigned ahead) : UmxSeparator(std::make_unique<Impl>(), ahead) {}
UmxSeparator::UmxSeparator(std::unique_ptr<Impl> loaded, unsigned ahead) : MaskSeparator(loaded->frames, ahead), impl(std::move(loaded)) {}
UmxSeparator::~UmxSeparator() = default;
void UmxSeparator::estimate(const std::vector<float>&, std::vector<float>&) {}
std::unique_ptr<LiveSeparator> openLiveSeparator(const std::string& path, unsigned) { return std::make_unique<StreamingSeparator>(path); }
#endif
}
