#include "swapchain.hpp"

#include <dwmapi.h>

#include <cmath>

Swapchain::~Swapchain() {
    if (waitable_) CloseHandle(waitable_);
}

void Swapchain::create(D3DContext& d3d, HWND hwnd, Size2D size) {
    d3d_     = &d3d;
    tearing_ = d3d.tearing_supported();
    flags_   = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
             (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width            = std::max(1u, size.width);
    desc.Height           = std::max(1u, size.height);
    desc.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount      = 3;
    desc.Scaling          = DXGI_SCALING_NONE;
    desc.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags            = flags_;

    ComPtr<IDXGISwapChain1> sc1;
    check_hr(d3d.factory()->CreateSwapChainForHwnd(d3d.device(), hwnd, &desc, nullptr, nullptr, &sc1),
             "CreateSwapChainForHwnd");
    d3d.factory()->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    check_hr(sc1.As(&swap_), "IDXGISwapChain2");
    swap_->SetMaximumFrameLatency(1);
    waitable_ = swap_->GetFrameLatencyWaitableObject();
    size_     = {desc.Width, desc.Height};
    create_rtv();
}

void Swapchain::create_rtv() {
    ComPtr<ID3D11Texture2D> back;
    check_hr(swap_->GetBuffer(0, IID_PPV_ARGS(&back)), "GetBuffer");
    check_hr(d3d_->device()->CreateRenderTargetView(back.Get(), nullptr, &rtv_), "CreateRenderTargetView");
}

void Swapchain::resize(Size2D size) {
    if (!swap_ || size.empty() || size == size_) return;
    d3d_->ctx()->OMSetRenderTargets(0, nullptr, nullptr);  // aucune référence au back buffer
    rtv_.Reset();
    d3d_->ctx()->Flush();
    check_hr(swap_->ResizeBuffers(0, size.width, size.height, DXGI_FORMAT_UNKNOWN, flags_), "ResizeBuffers");
    size_ = size;
    create_rtv();
}

double Swapchain::next_vblank(double now, double fallback_period) const {
    static const double inv_freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    double period = 0.0, last = 0.0;

    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof(ti);
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.qpcRefreshPeriod > 0) {
        period = double(ti.qpcRefreshPeriod) * inv_freq;
        last   = double(ti.qpcVBlank) * inv_freq;
    }
    // Phase du moniteur qui affiche réellement notre swapchain (DWM = moniteur principal).
    DXGI_FRAME_STATISTICS fs{};
    if (swap_ && SUCCEEDED(swap_->GetFrameStatistics(&fs)) && fs.SyncQPCTime.QuadPart > 0)
        last = double(fs.SyncQPCTime.QuadPart) * inv_freq;
    if (period <= 0.0) period = fallback_period;
    if (period <= 0.0 || last <= 0.0 || now - last > 1.0) return now;  // pas d'info fiable

    // Plus petit vblank >= now (last peut être légèrement dans le futur selon la source).
    double next = last + std::ceil((now - last) / period) * period;
    if (next - now < 1e-4) next += period;
    return next;
}

void Swapchain::present(bool vsync, bool allow_tearing) {
    const UINT flags = (!vsync && allow_tearing && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(vsync ? 1 : 0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        throw_hr(d3d_->device()->GetDeviceRemovedReason(), "Present (GPU perdu)");
}
