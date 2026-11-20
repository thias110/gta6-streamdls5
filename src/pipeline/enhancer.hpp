#pragma once
#include "compositor.hpp"
#include "enhance_logic.hpp"
#include "motion.hpp"

// Amélioration d'image (--enhance), passes plein écran en BGRA8 (le format du pipeline : aucune conversion).
//  - étage A, denoise() : filtre bilatéral 5x5 sur la capture, avant l'upscale (VSR, bicubique...) ;
//  - étage B, finish() : netteté adaptative sans dépassement, clarté, courbe de tons, saturation, dithering,
//    sur l'image upscalée (une fois par frame unique : les images interpolées en héritent).
// Tout est écrit pour rpenhance (MIT) ; aucune dépendance NVIDIA ni modèle.
class Enhancer {
public:
    void init(D3DContext& d3d, Compositor& comp);

    // Étage A : renvoie une texture de la taille de `src` (réutilisée d'un appel à l'autre).
    const GpuTexture& denoise(const GpuTexture& src, float strength);

    struct FinishInputs {
        const LumaPyramid*        cur  = nullptr;  // pyramide de la capture (niveau 0 : HUD, niveau 1 : clarté)
        const LumaPyramid*        prev = nullptr;  // frame unique précédente du même plan (stabilité du HUD)
        ID3D11ShaderResourceView* edges = nullptr;  // masque de bords dilaté déjà calculé (sinon Sobel en shader)
        bool                      hud_protect = true;
    };
    // Vrai si l'étage B a quelque chose à faire avec ces réglages.
    static bool finish_needed(const enhance::Params& p) {
        return p.sharpen > 0.0f || p.clarity > 0.0f || p.tone > 0.0f || p.saturation != 1.0f;
    }
    // Étage B : renvoie une texture de la taille de `up` (réutilisée d'un appel à l'autre).
    const GpuTexture& finish(const GpuTexture& up, const enhance::Params& p, const FinishInputs& in);

private:
    D3DContext*               d3d_  = nullptr;
    Compositor*               comp_ = nullptr;
    ComPtr<ID3D11PixelShader> ps_denoise_, ps_finish_;
    GpuTexture                denoised_, finished_;
};
