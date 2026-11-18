#pragma once
#include "../gpu/d3d_context.hpp"

// Swapchain flip-model, latence max 1 frame, objet "waitable" pour caler le rendu sur le vblank,
// tearing autorisé (VRR / G-Sync) quand le GPU le permet.
class Swapchain {
public:
    ~Swapchain();
    void create(D3DContext& d3d, HWND hwnd, Size2D size);
    void resize(Size2D size);
    void present(bool vsync, bool allow_tearing);

    ID3D11RenderTargetView* rtv() const { return rtv_.Get(); }
    HANDLE                  waitable() const { return waitable_; }
    Size2D                  size() const { return size_; }
    bool                    valid() const { return swap_ != nullptr; }

private:
    void create_rtv();

    D3DContext*                    d3d_ = nullptr;
    ComPtr<IDXGISwapChain2>        swap_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE                         waitable_ = nullptr;
    Size2D                         size_{};
    UINT                           flags_   = 0;
    bool                           tearing_ = false;
};
