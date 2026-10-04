#pragma once
#include <Accelerate/Accelerate.h>
#include <onnxruntime_cxx_api.h>
#include <array>
#include <vector>
#include <stdexcept>
#include <type_traits>

namespace ws {
template<class T> struct LiveDft {
    static constexpr bool wide = std::is_same_v<T, double>;
    using Setup = std::conditional_t<wide, FFTSetupD, FFTSetup>;
    Setup setup;
    bool inverse, one;
    std::array<T, 1024> re{}, im{};
    LiveDft(const OrtKernelInfo* raw) {
        Ort::ConstKernelInfo info(raw);
        inverse = info.GetAttribute<int64_t>("inverse"); one = info.GetAttribute<int64_t>("onesided");
        if (info.GetAttribute<int64_t>("axis") != 1) throw std::runtime_error("Unsupported DFT axis");
        if constexpr(wide) setup = vDSP_create_fftsetupD(10, kFFTRadix2);
        else setup = vDSP_create_fftsetup(10, kFFTRadix2);
        if (!setup) throw std::runtime_error("DFT setup failed");
    }
    ~LiveDft() {
        if constexpr(wide) vDSP_destroy_fftsetupD(setup); else vDSP_destroy_fftsetup(setup);
    }
    void Compute(OrtKernelContext* raw) {
        Ort::KernelContext context(raw);
        auto input = context.GetInput(0);
        auto shape = input.GetTensorTypeAndShapeInfo().GetShape();
        if (shape.size()!=3 || shape[1]!=1024 || (shape[2]!=1 && shape[2]!=2) || *context.GetInput(1).GetTensorData<int64_t>()!=1024)
            throw std::runtime_error("Unsupported DFT shape");
        auto* src=input.GetTensorData<T>();
        int64_t count=one?513:1024;
        std::vector<int64_t> outShape{shape[0],count,2};
        auto out=context.GetOutput(0,outShape);
        T* dst=out.GetTensorMutableData<T>();
        for(int64_t b=0;b<shape[0];++b){
            for(int64_t i=0;i<1024;++i){re[i]=src[(b*1024+i)*shape[2]]; im[i]=shape[2]==2?src[(b*1024+i)*2+1]:0;}
            if constexpr(wide){ DSPDoubleSplitComplex z{re.data(),im.data()}; vDSP_fft_zipD(setup,&z,1,10,inverse?FFT_INVERSE:FFT_FORWARD); }
            else { DSPSplitComplex z{re.data(),im.data()}; vDSP_fft_zip(setup,&z,1,10,inverse?FFT_INVERSE:FFT_FORWARD); }
            T scale=inverse?T(1./1024):T(1);
            for(int64_t i=0;i<count;++i){dst[(b*count+i)*2]=re[i]*scale; dst[(b*count+i)*2+1]=im[i]*scale;}
        }
    }
};
template<class T> struct LiveDftOp : Ort::CustomOpBase<LiveDftOp<T>,LiveDft<T>> {
    void* CreateKernel(const OrtApi&,const OrtKernelInfo* info) const {return new LiveDft<T>(info);}
    const char* GetName() const {return "DFT";}
    size_t GetInputTypeCount() const {return 2;}
    ONNXTensorElementDataType GetInputType(size_t i) const {return i?ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:(std::is_same_v<T,double>?ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);}
    size_t GetOutputTypeCount() const {return 1;}
    ONNXTensorElementDataType GetOutputType(size_t) const {return GetInputType(0);}
};

template<class T> struct LiveMatMul {
    LiveMatMul(const OrtKernelInfo*) {}
    void Compute(OrtKernelContext* raw) {
        Ort::KernelContext ctx(raw);
        auto a=ctx.GetInput(0), b=ctx.GetInput(1);
        auto as=a.GetTensorTypeAndShapeInfo().GetShape(), bs=b.GetTensorTypeAndShapeInfo().GetShape();
        if(as.size()<2 || bs.size()!=2 || as.back()!=bs[0]) throw std::runtime_error("Unsupported live matrix shape");
        int rows=1, k=int(bs[0]), n=int(bs[1]);
        for(size_t i=0;i+1<as.size();++i) rows*=int(as[i]);
        as.back()=n;
        auto out=ctx.GetOutput(0,as);
        if constexpr(std::is_same_v<T,double>) cblas_dgemm(CblasRowMajor,CblasNoTrans,CblasNoTrans,rows,n,k,1,a.GetTensorData<T>(),k,b.GetTensorData<T>(),n,0,out.GetTensorMutableData<T>(),n);
        else cblas_sgemm(CblasRowMajor,CblasNoTrans,CblasNoTrans,rows,n,k,1,a.GetTensorData<T>(),k,b.GetTensorData<T>(),n,0,out.GetTensorMutableData<T>(),n);
    }
};
template<class T> struct LiveMatMulOp : Ort::CustomOpBase<LiveMatMulOp<T>,LiveMatMul<T>> {
    void* CreateKernel(const OrtApi&,const OrtKernelInfo* info) const {return new LiveMatMul<T>(info);}
    const char* GetName() const {return "MatMul";}
    size_t GetInputTypeCount() const {return 2;}
    ONNXTensorElementDataType GetInputType(size_t) const {return std::is_same_v<T,double>?ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;}
    size_t GetOutputTypeCount() const {return 1;}
    ONNXTensorElementDataType GetOutputType(size_t) const {return GetInputType(0);}
};
}
