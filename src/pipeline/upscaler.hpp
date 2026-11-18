#pragma once
#include "../config.hpp"
#include "compositor.hpp"

// Upscale de la capture vers la taille d'affichage.
//  - Vsr     : VideoProcessor D3D11 (BGRA -> NV12 -> BGRA) avec l'extension NVIDIA "RTX Video
//              Super Resolution", la même voie que Chrome/VLC/mpv. Aucun SDK à télécharger ;
//              il faut activer "Super Resolution" dans NVIDIA App > Vidéo.
//  - Bicubic : Catmull-Rom + netteté anti-halo (shader, tous GPU)
//  - Off     : bilinéaire
class Upscaler {
public:
    enum class Path { Bilinear, Bicubic, DriverScaler, Vsr };

    void init(D3DContext& d3d, Compositor& comp);
    const GpuTexture& run(const GpuTexture& src, Size2D out, UpscaleMode mode, float sharpness);

    Path        last_path() const { return last_path_; }
    const char* vsr_status() const { return vsr_status_; }
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
    ComPtr<ID3D11PixelShader> ps_upscale_;
    GpuTexture                out_;
    Vsr                       vsr_;
    Path                      last_path_  = Path::Bilinear;
    const char*               vsr_status_ = "pas encore utilisé";
};
