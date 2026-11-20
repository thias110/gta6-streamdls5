#pragma once
#include "../config.hpp"
#include "../nvvfx/nvvfx_backend.hpp"
#include "compositor.hpp"

// Upscale de la capture vers la taille d'affichage.
//  - Vsr     : VideoProcessor D3D11 (BGRA -> NV12 -> BGRA) avec l'extension NVIDIA "RTX Video
//              Super Resolution", la même voie que Chrome/VLC/mpv. Aucun SDK à télécharger ;
//              il faut activer "Super Resolution" dans NVIDIA App > Vidéo.
//  - Bicubic : Catmull-Rom + netteté anti-halo (shader, tous GPU)
//  - Off     : bilinéaire
//  - Nvvfx   : VSR du NVIDIA Video Effects SDK (build RPENHANCE_NVVFX, SDK installé) ; en cas d'échec
//              (SDK absent, GPU/feature non supportés...) repli sur Vsr puis sur Bicubic.
class Upscaler {
public:
    enum class Path { Bilinear, Bicubic, DriverScaler, Vsr, NvvfxVsr };

    // `nvvfx` : backend partagé avec le pipeline (nullptr = mode Nvvfx traité comme Vsr).
    void init(D3DContext& d3d, Compositor& comp, NvvfxBackend* nvvfx = nullptr);
    const GpuTexture& run(const GpuTexture& src, Size2D out, UpscaleMode mode, float sharpness);

    Path        last_path() const { return last_path_; }
    // État de la voie demandée : backend NVIDIA VFX en mode Nvvfx, VideoProcessor sinon.
    const char* vsr_status() const { return nvvfx_active_status_ ? nvvfx_active_status_ : vsr_status_; }
    static const char* path_name(Path p);

private:
    bool run_vsr(const GpuTexture& src);
    bool setup_vsr(Size2D in, Size2D out);

    struct Vsr {
        Size2D in{}, out{};
        ComPtr<ID3D11VideoProcessorEnumerator> conv_enum, scale_enum;
        ComPtr<ID3D11VideoProcessor>           conv, scale;
        ComPtr<ID3D11Texture2D>                nv12;
        ComPtr<ID3D11VideoProcessorOutputView> nv12_out, final_out;
        ComPtr<ID3D11VideoProcessorInputView>  nv12_in, src_in;
        ComPtr<ID3D11Texture2D>                src_ref, out_ref;
        bool configured = false;
        bool failed     = false;
        bool ext_ok     = false;
    };

    D3DContext*               d3d_  = nullptr;
    Compositor*               comp_ = nullptr;
    NvvfxBackend*             nvvfx_ = nullptr;
    const char*               nvvfx_active_status_ = nullptr;  // non nul quand le mode Nvvfx est demandé
    bool                      nvvfx_fallback_logged_ = false;
    ComPtr<ID3D11PixelShader> ps_upscale_;
    GpuTexture                out_;
    Vsr                       vsr_;
    Path                      last_path_  = Path::Bilinear;
    const char*               vsr_status_ = "pas encore utilisé";
};
