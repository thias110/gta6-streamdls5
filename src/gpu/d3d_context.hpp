#pragma once
#include "../common.hpp"

struct GpuTexture {
    ComPtr<ID3D11Texture2D>           tex;
    ComPtr<ID3D11ShaderResourceView>  srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11RenderTargetView>    rtv;
    uint32_t    width  = 0;
    uint32_t    height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    Size2D size() const { return {width, height}; }
    bool   valid() const { return tex != nullptr; }
    bool   matches(uint32_t w, uint32_t h, DXGI_FORMAT f) const {
        return tex && width == w && height == h && format == f;
    }
    void reset() { *this = GpuTexture{}; }
    // (Re)crée la texture si taille/format diffèrent. Renvoie true si recréée.
    bool ensure(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT f, UINT bind, const char* name);
};

class D3DContext {
public:
    void initialize(bool debug_layer);

    ID3D11Device*         device() const { return device_.Get(); }
    ID3D11DeviceContext1* ctx() const { return ctx_.Get(); }
    ID3D11VideoDevice*    video_device() const { return video_device_.Get(); }
    ID3D11VideoContext*   video_ctx() const { return video_ctx_.Get(); }
    IDXGIFactory2*        factory() const { return factory_.Get(); }

    bool               tearing_supported() const { return tearing_; }
    bool               is_nvidia() const { return vendor_id_ == 0x10DE; }
    const std::string& adapter_name() const { return adapter_name_; }

    // Compile `body` précédé du bloc HLSL commun (shaders::kCommon).
    ComPtr<ID3D11ComputeShader> make_cs(std::string_view body, const char* entry) const;
    ComPtr<ID3D11PixelShader>   make_ps(std::string_view body, const char* entry) const;

    // s0 = linéaire clamp, s1 = point clamp
    void bind_samplers_cs() const;
    void bind_samplers_ps() const;

    // Écrit dans un constant buffer dynamique (WRITE_DISCARD) et le renvoie.
    ID3D11Buffer* constants(const void* data, size_t bytes) const;
    template <class T>
    ID3D11Buffer* constants(const T& v) const {
        static_assert(sizeof(T) % 16 == 0, "constant buffer : taille multiple de 16");
        return constants(&v, sizeof(T));
    }

    // Triangle plein écran (SV_VertexID), le pixel shader doit être déjà lié.
    void draw_fullscreen() const;
    void unbind_cs() const;

private:
    ComPtr<IDXGIAdapter1>        adapter_;
    ComPtr<IDXGIFactory2>        factory_;
    ComPtr<ID3D11Device>         device_;
    ComPtr<ID3D11DeviceContext1> ctx_;
    ComPtr<ID3D11VideoDevice>    video_device_;
    ComPtr<ID3D11VideoContext>   video_ctx_;
    ComPtr<ID3D11VertexShader>   fullscreen_vs_;
    ComPtr<ID3D11SamplerState>   linear_, point_;
    ComPtr<ID3D11Buffer>         cb_;
    bool        tearing_   = false;
    uint32_t    vendor_id_ = 0;
    std::string adapter_name_;
};
