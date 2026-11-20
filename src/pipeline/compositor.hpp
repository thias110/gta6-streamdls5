#pragma once
#include "../gpu/d3d_context.hpp"

#include <initializer_list>

struct SynthParams {
    float static_lo = 0.02f;  // |A-B| sous lequel un pixel est considéré fixe
    float static_hi = 0.06f;
    float err_lo    = 0.06f;  // désaccord des échantillons compensés à partir duquel on se méfie du vecteur
    float err_hi    = 0.20f;
};

// Passage UV image -> UV de la grille de flow (blocs 8x8 : la grille déborde de l'image quand
// la taille n'est pas multiple de 8). Voir LumaPyramid::flow_scale().
struct FlowScale {
    float x = 1.0f;
    float y = 1.0f;
};

// Passes plein écran (pixel shaders) dans une render target.
class Compositor {
public:
    void init(D3DContext& d3d);

    void begin(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp);
    void end();
    void scissor(const D3D11_RECT* r);  // nullptr = désactivé

    void draw(ID3D11PixelShader* ps, std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11Buffer* cb = nullptr);
    void blit(ID3D11ShaderResourceView* src);
    void synth(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, ID3D11ShaderResourceView* flow_ab,
               ID3D11ShaderResourceView* flow_ba, float t, const SynthParams& p, FlowScale fs);
    void flow_viz(ID3D11ShaderResourceView* img, ID3D11ShaderResourceView* flow, float scale, FlowScale fs);

    D3DContext& d3d() const { return *d3d_; }

private:
    D3DContext*                     d3d_ = nullptr;
    ComPtr<ID3D11PixelShader>       blit_, synth_, viz_;
    ComPtr<ID3D11RasterizerState>   rs_plain_, rs_scissor_;
};
