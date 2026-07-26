#pragma once
#include "separation.hpp"
namespace ws {
class OnnxSeparator {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    OnnxSeparator(const std::string& path, int stems);
    ~OnnxSeparator();
    Stems separate(const Stereo&, const std::function<void(float)>&);
    const char* backend() const;
};
}
