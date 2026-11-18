#include "swapchain.hpp"

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

void Swapchain::present(bool vsync, bool allow_tearing) {
    const UINT flags = (!vsync && allow_tearing && tearing_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    HRESULT hr = swap_->Present(vsync ? 1 : 0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        throw_hr(d3d_->device()->GetDeviceRemovedReason(), "Present (GPU perdu)");
}
