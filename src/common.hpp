#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <unknwn.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

struct Size2D {
    uint32_t width{};
    uint32_t height{};
    bool operator==(const Size2D&) const = default;
    bool empty() const { return width == 0 || height == 0; }
};

// Journal : console parente (si lancée depuis un terminal), OutputDebugString et rpenhance.log.
void log_init(const wchar_t* path);
void log_msg(const char* fmt, ...);

[[noreturn]] void throw_hr(long hr, const char* ctx);
inline void check_hr(HRESULT hr, const char* ctx) {
    if (FAILED(hr)) throw_hr(hr, ctx);
}

std::string  narrow(std::wstring_view w);
std::wstring widen(std::string_view s);
std::wstring to_lower(std::wstring s);
std::wstring exe_directory();

// Horloge QPC en secondes : même base que Direct3D11CaptureFrame::SystemRelativeTime.
double now_seconds();

inline uint32_t div_up(uint32_t n, uint32_t d) { return (n + d - 1) / d; }
