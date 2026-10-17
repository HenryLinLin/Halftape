#pragma once
#if __has_cpp_attribute(clang::nonblocking)
#define WS_REALTIME [[clang::nonblocking]]
#else
#define WS_REALTIME
#endif
namespace ws {
void realtimePolicy(double periodSeconds, double computationSeconds) noexcept;
}
