#include "upscaler.hpp"
#include "../gpu/shaders.hpp"

namespace {
// Extension "Post Processing Engine" du driver NVIDIA (RTX Video Super Resolution).
constexpr GUID kNvidiaPpeInterface = {0xd43ce1b3, 0x1f4b, 0x48ac, {0xba, 0xee, 0xc3, 0xc2, 0x53, 0x75, 0xe6, 0xf7}};
struct NvidiaVsrExtension {
    UINT version;  // 1
    UINT method;   // 2 = super resolution
    UINT enable;
};
}  // namespace

const char* Upscaler::path_name(Path p) {
    switch (p) {
    case Path::Bilinear: return "bilinéaire";
    case Path::Bicubic: return "bicubique + netteté";
    case Path::DriverScaler: return "scaler vidéo du driver (sans VSR)";
    case Path::Vsr: return "RTX Video Super Resolution";
    }
    return "?";
}

void Upscaler::init(D3DContext& d3d, Compositor& comp) {
    d3d_        = &d3d;
    comp_       = &comp;
    ps_upscale_ = d3d.make_ps(shaders::kUpscale, "ps_upscale");
}

const GpuTexture& Upscaler::run(const GpuTexture& src, Size2D out, UpscaleMode mode, float sharpness) {
    out_.ensure(d3d_->device(), out.width, out.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "upscale out");

    if (mode == UpscaleMode::Vsr && run_vsr(src)) {
        last_path_ = vsr_.ext_ok ? Path::Vsr : Path::DriverScaler;
        return out_;
    }

    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(out.width), float(out.height), 0.0f, 1.0f};
    comp_->begin(out_.rtv.Get(), vp);
    if (mode == UpscaleMode::Off) {
        comp_->blit(src.srv.Get());
        last_path_ = Path::Bilinear;
    } else {
        struct {
            float sw, sh, isw, ish, sharp, pad[3];
        } cb{float(src.width), float(src.height), 1.0f / float(src.width), 1.0f / float(src.height), sharpness, {}};
        comp_->draw(ps_upscale_.Get(), {src.srv.Get()}, d3d_->constants(cb));
        last_path_ = Path::Bicubic;
    }
    comp_->end();
    return out_;
}

bool Upscaler::setup_vsr(Size2D in, Size2D out) {
    auto* dev  = d3d_->device();
    auto* vdev = d3d_->video_device();
    auto* vctx = d3d_->video_ctx();

    auto make = [&](Size2D i, Size2D o, ComPtr<ID3D11VideoProcessorEnumerator>& en,
                    ComPtr<ID3D11VideoProcessor>& vp) {
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate   = {60, 1};
        cd.InputWidth       = i.width;
        cd.InputHeight      = i.height;
        cd.OutputFrameRate  = {60, 1};
        cd.OutputWidth      = o.width;
        cd.OutputHeight     = o.height;
        cd.Usage            = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        return SUCCEEDED(vdev->CreateVideoProcessorEnumerator(&cd, &en)) &&
               SUCCEEDED(vdev->CreateVideoProcessor(en.Get(), 0, &vp));
    };
    if (!make(in, in, vsr_.conv_enum, vsr_.conv) || !make(in, out, vsr_.scale_enum, vsr_.scale)) {
        vsr_status_ = "VideoProcessor D3D11 indisponible";
        return false;
    }

    auto supports = [](ID3D11VideoProcessorEnumerator* e, DXGI_FORMAT f, UINT need) {
        UINT flags = 0;
        return SUCCEEDED(e->CheckVideoProcessorFormat(f, &flags)) && (flags & need) == need;
    };
    if (!supports(vsr_.conv_enum.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !supports(vsr_.conv_enum.Get(), DXGI_FORMAT_NV12, D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) ||
        !supports(vsr_.scale_enum.Get(), DXGI_FORMAT_NV12, D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
        !supports(vsr_.scale_enum.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
        vsr_status_ = "formats BGRA/NV12 non supportés par le VideoProcessor";
        return false;
    }

    // Intermédiaire NV12 : le flux PS5 est déjà en 4:2:0, on ne perd rien, et c'est le format
    // vidéo sur lequel le driver applique VSR.
    D3D11_TEXTURE2D_DESC d{};
    d.Width            = in.width;
    d.Height           = in.height;
    d.MipLevels        = 1;
    d.ArraySize        = 1;
    d.Format           = DXGI_FORMAT_NV12;
    d.SampleDesc.Count = 1;
    d.Usage            = D3D11_USAGE_DEFAULT;
    d.BindFlags        = D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &vsr_.nv12))) {
        vsr_status_ = "texture NV12 impossible";
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(vdev->CreateVideoProcessorOutputView(vsr_.nv12.Get(), vsr_.conv_enum.Get(), &ovd, &vsr_.nv12_out)) ||
        FAILED(vdev->CreateVideoProcessorInputView(vsr_.nv12.Get(), vsr_.scale_enum.Get(), &ivd, &vsr_.nv12_in))) {
        vsr_status_ = "vues NV12 impossibles";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
    rgb.RGB_Range     = 0;  // 0-255
    rgb.YCbCr_Matrix  = 1;  // BT.709
    rgb.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
    yuv.YCbCr_Matrix  = 1;
    yuv.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;

    const RECT rin{0, 0, LONG(in.width), LONG(in.height)};
    const RECT rout{0, 0, LONG(out.width), LONG(out.height)};
    for (auto* vp : {vsr_.conv.Get(), vsr_.scale.Get()}) {
        vctx->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        vctx->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
    }
    vctx->VideoProcessorSetStreamColorSpace(vsr_.conv.Get(), 0, &rgb);
    vctx->VideoProcessorSetOutputColorSpace(vsr_.conv.Get(), &yuv);
    vctx->VideoProcessorSetStreamSourceRect(vsr_.conv.Get(), 0, TRUE, &rin);
    vctx->VideoProcessorSetStreamDestRect(vsr_.conv.Get(), 0, TRUE, &rin);
    vctx->VideoProcessorSetOutputTargetRect(vsr_.conv.Get(), TRUE, &rin);

    vctx->VideoProcessorSetStreamColorSpace(vsr_.scale.Get(), 0, &yuv);
    vctx->VideoProcessorSetOutputColorSpace(vsr_.scale.Get(), &rgb);
    vctx->VideoProcessorSetStreamSourceRect(vsr_.scale.Get(), 0, TRUE, &rin);
    vctx->VideoProcessorSetStreamDestRect(vsr_.scale.Get(), 0, TRUE, &rout);
    vctx->VideoProcessorSetOutputTargetRect(vsr_.scale.Get(), TRUE, &rout);

    NvidiaVsrExtension ext{1, 2, 1};
    HRESULT hr = vctx->VideoProcessorSetStreamExtension(vsr_.scale.Get(), 0, &kNvidiaPpeInterface, sizeof(ext), &ext);
    vsr_.ext_ok = SUCCEEDED(hr) && d3d_->is_nvidia();
    if (vsr_.ext_ok)
        vsr_status_ = "extension acceptée : active \"Super Resolution\" dans NVIDIA App > Système > Vidéo";
    else if (d3d_->is_nvidia())
        vsr_status_ = "extension VSR refusée par le driver (driver trop ancien ?)";
    else
        vsr_status_ = "GPU non NVIDIA : scaler vidéo standard du driver";
    log_msg("Upscale VideoProcessor %ux%u -> %ux%u : %s (hr=0x%08lX)", in.width, in.height, out.width, out.height,
            vsr_status_, static_cast<unsigned long>(hr));
    return true;
}

bool Upscaler::run_vsr(const GpuTexture& src) {
    auto* vdev = d3d_->video_device();
    auto* vctx = d3d_->video_ctx();
    if (!vdev || !vctx) {
        vsr_status_ = "API vidéo D3D11 indisponible";
        return false;
    }

    const Size2D in = src.size(), out = out_.size();
    if (!vsr_.configured || !(vsr_.in == in) || !(vsr_.out == out)) {
        vsr_            = Vsr{};
        vsr_.in         = in;
        vsr_.out        = out;
        vsr_.configured = true;
        vsr_.failed     = !setup_vsr(in, out);
    }
    if (vsr_.failed) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (vsr_.src_ref.Get() != src.tex.Get()) {
        vsr_.src_in.Reset();
        if (FAILED(vdev->CreateVideoProcessorInputView(src.tex.Get(), vsr_.conv_enum.Get(), &ivd, &vsr_.src_in))) {
            vsr_status_ = "vue d'entrée BGRA impossible";
            vsr_.failed = true;
            return false;
        }
        vsr_.src_ref = src.tex;
    }
    if (vsr_.out_ref.Get() != out_.tex.Get()) {
        vsr_.final_out.Reset();
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(vdev->CreateVideoProcessorOutputView(out_.tex.Get(), vsr_.scale_enum.Get(), &ovd, &vsr_.final_out))) {
            vsr_status_ = "vue de sortie BGRA impossible";
            vsr_.failed = true;
            return false;
        }
        vsr_.out_ref = out_.tex;
    }

    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable        = TRUE;
    st.pInputSurface = vsr_.src_in.Get();
    if (FAILED(vctx->VideoProcessorBlt(vsr_.conv.Get(), vsr_.nv12_out.Get(), 0, 1, &st))) {
        vsr_status_ = "VideoProcessorBlt BGRA->NV12 en échec";
        vsr_.failed = true;
        return false;
    }
    st.pInputSurface = vsr_.nv12_in.Get();
    if (FAILED(vctx->VideoProcessorBlt(vsr_.scale.Get(), vsr_.final_out.Get(), 0, 1, &st))) {
        vsr_status_ = "VideoProcessorBlt NV12->BGRA en échec";
        vsr_.failed = true;
        return false;
    }
    return true;
}
