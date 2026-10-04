#include "wscore/streaming.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#ifdef WS_ONNX
#define ACCELERATE_NEW_LAPACK
#include <onnxruntime_cxx_api.h>
#include "live_math.hpp"
#endif

namespace ws {
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
        std::ifstream file(path, std::ios::binary);
        if (!file) throw std::runtime_error("Choose the streaming Sing model or run scripts/sing-model.sh.");
        CC_SHA256_CTX hash; CC_SHA256_Init(&hash);
        std::array<char, 65536> bytes{};
        while (file.read(bytes.data(), bytes.size()) || file.gcount()) CC_SHA256_Update(&hash, bytes.data(), CC_LONG(file.gcount()));
        unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(digest, &hash);
        std::ostringstream hex;
        for (auto c : digest) hex << std::hex << std::setfill('0') << std::setw(2) << unsigned(c);
        if (hex.str() != "77164d6a581fafb2a31f53fd8ffde44c07cf618472952a4cdba14e68dda3b8b9")
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
#else
struct StreamingSeparator::Impl {};
StreamingSeparator::StreamingSeparator(const std::string&) { throw std::runtime_error("Streaming Sing requires ONNX Runtime."); }
StreamingSeparator::~StreamingSeparator() = default;
void StreamingSeparator::reset() {}
bool StreamingSeparator::process(const LiveBlock&, LiveStems&) { return false; }
#endif
}
