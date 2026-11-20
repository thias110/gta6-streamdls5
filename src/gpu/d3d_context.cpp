#include "d3d_context.hpp"
#include "shaders.hpp"

#include <cstring>
#include <d3dcompiler.h>

bool GpuTexture::ensure(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT f, UINT bind, const char* name) {
    if (matches(w, h, f)) return false;
    reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width            = w;
    d.Height           = h;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = f;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_DEFAULT;
    d.BindFlags        = bind;
    check_hr(dev->CreateTexture2D(&d, nullptr, &tex), name);
    if (bind & D3D11_BIND_SHADER_RESOURCE) check_hr(dev->CreateShaderResourceView(tex.Get(), nullptr, &srv), name);
    if (bind & D3D11_BIND_UNORDERED_ACCESS) check_hr(dev->CreateUnorderedAccessView(tex.Get(), nullptr, &uav), name);
    if (bind & D3D11_BIND_RENDER_TARGET) check_hr(dev->CreateRenderTargetView(tex.Get(), nullptr, &rtv), name);
    width  = w;
    height = h;
    format = f;
    return true;
}

namespace {
ComPtr<ID3DBlob> compile_hlsl(std::string_view body, const char* entry, const char* target) {
    std::string src;
    src.reserve(sizeof(shaders::kCommon) + body.size());
    src += shaders::kCommon;
    src += body;

    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompile(src.data(), src.size(), entry, nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::string msg = std::string("compilation HLSL ") + entry + " : ";
        if (errors) msg.append(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
        log_msg("%s", msg.c_str());
        throw std::runtime_error(msg);
    }
    return code;
}
}  // namespace

namespace {
// Échoue tout de suite, avec un message clair, s'il manque au device une fonctionnalité dont le pipeline a
// besoin (utile surtout en --warp : rasteriseur logiciel de la CI, sans GPU).
void require_features(ID3D11Device* dev, const std::string& adapter) {
    auto fail = [&](const std::string& what) {
        const std::string msg = "fonctionnalité D3D11 manquante sur " + adapter + " : " + what;
        log_msg("%s", msg.c_str());
        throw std::runtime_error(msg);
    };
    if (dev->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) fail("feature level 11_0 (compute shaders cs_5_0)");

    struct Need {
        DXGI_FORMAT f;
        UINT        flags;
        const char* name;
    };
    // Écritures UAV typées seulement (aucune lecture de UAV typé dans les shaders).
    const Need needs[] = {
        {DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE,
         "B8G8R8A8_UNORM (cible de rendu, échantillonnage)"},
        {DXGI_FORMAT_R16_FLOAT, D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW | D3D11_FORMAT_SUPPORT_SHADER_LOAD,
         "R16_FLOAT (UAV typé : pyramide de luminance, bords)"},
        {DXGI_FORMAT_R32G32_FLOAT, D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW | D3D11_FORMAT_SUPPORT_SHADER_LOAD,
         "R32G32_FLOAT (UAV typé : flow)"},
    };
    for (const Need& n : needs) {
        UINT got = 0;
        if (FAILED(dev->CheckFormatSupport(n.f, &got)) || (got & n.flags) != n.flags) fail(n.name);
    }
}
}  // namespace

void D3DContext::initialize(bool debug_layer, bool warp) {
    warp_ = warp;
    ComPtr<IDXGIFactory6> f6;
    if (!warp) {
        check_hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&f6)), "CreateDXGIFactory2");
        check_hr(f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter_)),
                 "EnumAdapterByGpuPreference");
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT base = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    ComPtr<ID3D11Device>        dev;
    ComPtr<ID3D11DeviceContext> ctx;
    auto create = [&](UINT flags) {
        dev.Reset();
        ctx.Reset();
        // WARP : pas d'adaptateur explicite, D3D choisit le rasteriseur logiciel.
        return D3D11CreateDevice(warp ? nullptr : adapter_.Get(), warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_UNKNOWN,
                                 nullptr, flags, levels, UINT(std::size(levels)), D3D11_SDK_VERSION, &dev, nullptr,
                                 &ctx);
    };
    HRESULT hr = E_FAIL;
    if (debug_layer) hr = create(base | D3D11_CREATE_DEVICE_DEBUG);  // échoue si les Graphics Tools manquent
    if (FAILED(hr)) hr = create(base);
    if (FAILED(hr)) hr = create(D3D11_CREATE_DEVICE_BGRA_SUPPORT);
    check_hr(hr, warp ? "D3D11CreateDevice (WARP)" : "D3D11CreateDevice");

    if (warp) {  // adaptateur et fabrique DXGI du device WARP (la swapchain doit venir de la même fabrique)
        ComPtr<IDXGIDevice>  dxgi_dev;
        ComPtr<IDXGIAdapter> a;
        check_hr(dev.As(&dxgi_dev), "IDXGIDevice (WARP)");
        check_hr(dxgi_dev->GetAdapter(&a), "IDXGIDevice::GetAdapter (WARP)");
        check_hr(a.As(&adapter_), "IDXGIAdapter1 (WARP)");
        check_hr(adapter_->GetParent(IID_PPV_ARGS(&f6)), "IDXGIFactory6 (WARP)");
    }
    DXGI_ADAPTER_DESC1 ad{};
    adapter_->GetDesc1(&ad);
    adapter_name_ = narrow(ad.Description);
    vendor_id_    = ad.VendorId;
    const unsigned fl = unsigned(dev->GetFeatureLevel());
    log_msg("Device D3D11 : %s, feature level %u_%u%s", adapter_name_.c_str(), (fl >> 12) & 0xFu, (fl >> 8) & 0xFu,
            warp ? ", WARP (rendu logiciel)" : "");
    require_features(dev.Get(), adapter_name_);

    device_ = dev;
    check_hr(ctx.As(&ctx_), "ID3D11DeviceContext1");
    dev.As(&video_device_);  // optionnels : nécessaires seulement pour RTX VSR
    ctx.As(&video_ctx_);

    // WGC et le driver vidéo peuvent toucher le device depuis d'autres threads.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);

    check_hr(f6.As(&factory_), "IDXGIFactory2");
    ComPtr<IDXGIFactory5> f5;
    BOOL allow = FALSE;
    if (SUCCEEDED(f6.As(&f5)) &&
        SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))))
        tearing_ = allow != FALSE;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    check_hr(device_->CreateSamplerState(&sd, &linear_), "sampler linear");
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    check_hr(device_->CreateSamplerState(&sd, &point_), "sampler point");

    auto vs = compile_hlsl(shaders::kFullscreenVS, "vs_fullscreen", "vs_5_0");
    check_hr(device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &fullscreen_vs_),
             "CreateVertexShader");

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = 256;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    check_hr(device_->CreateBuffer(&bd, nullptr, &cb_), "constant buffer");

    log_msg("GPU : %s (vendor 0x%04X), tearing %s, API vidéo %s", adapter_name_.c_str(), vendor_id_,
            tearing_ ? "oui" : "non", video_ctx_ ? "oui" : "non");
}

ComPtr<ID3D11ComputeShader> D3DContext::make_cs(std::string_view body, const char* entry) const {
    auto code = compile_hlsl(body, entry, "cs_5_0");
    ComPtr<ID3D11ComputeShader> cs;
    check_hr(device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs), entry);
    return cs;
}

ComPtr<ID3D11PixelShader> D3DContext::make_ps(std::string_view body, const char* entry) const {
    auto code = compile_hlsl(body, entry, "ps_5_0");
    ComPtr<ID3D11PixelShader> ps;
    check_hr(device_->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &ps), entry);
    return ps;
}

void D3DContext::bind_samplers_cs() const {
    ID3D11SamplerState* s[2] = {linear_.Get(), point_.Get()};
    ctx_->CSSetSamplers(0, 2, s);
}

void D3DContext::bind_samplers_ps() const {
    ID3D11SamplerState* s[2] = {linear_.Get(), point_.Get()};
    ctx_->PSSetSamplers(0, 2, s);
}

ID3D11Buffer* D3DContext::constants(const void* data, size_t bytes) const {
    D3D11_MAPPED_SUBRESOURCE m{};
    check_hr(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map constant buffer");
    std::memcpy(m.pData, data, std::min<size_t>(bytes, 256));
    ctx_->Unmap(cb_.Get(), 0);
    return cb_.Get();
}

void D3DContext::draw_fullscreen() const {
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(fullscreen_vs_.Get(), nullptr, 0);
    ctx_->Draw(3, 0);
}

void D3DContext::unbind_cs() const {
    // 6 SRV / 2 UAV : les noyaux de flow traitent les deux sens d'une paire dans un même Dispatch.
    ID3D11ShaderResourceView*  srv[6]{};
    ID3D11UnorderedAccessView* uav[2]{};
    ctx_->CSSetShaderResources(0, 6, srv);
    ctx_->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
    ctx_->CSSetShader(nullptr, nullptr, 0);
}
