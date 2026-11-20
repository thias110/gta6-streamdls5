#pragma once
#include "compositor.hpp"
#include "enhance_logic.hpp"

// Amélioration d'image (--enhance), passes plein écran en BGRA8 (le format du pipeline : aucune conversion).
//  - étage A, denoise() : filtre bilatéral 5x5 sur la capture, avant l'upscale (VSR, bicubique...).
// Tout est écrit pour rpenhance (MIT) ; aucune dépendance NVIDIA ni modèle.
class Enhancer {
public:
    void init(D3DContext& d3d, Compositor& comp);

    // Étage A : renvoie une texture de la taille de `src` (réutilisée d'un appel à l'autre).
    const GpuTexture& denoise(const GpuTexture& src, float strength);

private:
    D3DContext*               d3d_  = nullptr;
    Compositor*               comp_ = nullptr;
    ComPtr<ID3D11PixelShader> ps_denoise_;
    GpuTexture                denoised_;
};
