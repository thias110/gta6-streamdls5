#pragma once
#include "../gpu/d3d_context.hpp"
#include "compositor.hpp"
#include "scene_cut.hpp"

inline constexpr uint32_t kFlowBlock = 8;  // taille des blocs du flow (BS dans kMotion)

// Grille de flow d'un niveau de taille `s` : div_up(s, 8) blocs ; facteur UV image -> UV grille.
inline FlowScale flow_grid_scale(uint32_t w, uint32_t h) {
    const uint32_t gw = (w + kFlowBlock - 1) / kFlowBlock, gh = (h + kFlowBlock - 1) / kFlowBlock;
    if (!gw || !gh) return {};
    return {float(w) / float(gw * kFlowBlock), float(h) / float(gh * kFlowBlock)};
}

// Pyramide de luminance R16F ; niveau 0 = demi-résolution de la capture.
struct LumaPyramid {
    static constexpr int kMaxLevels = 5;
    std::vector<GpuTexture> levels;
    Size2D                  source{};

    void ensure(ID3D11Device* dev, Size2D src);
    bool valid() const { return !levels.empty(); }
    // Facteur à appliquer aux UV image pour lire le flow de niveau 0 (sortie de estimate_flow).
    FlowScale flow_scale() const { return valid() ? flow_grid_scale(levels[0].width, levels[0].height) : FlowScale{}; }
};

struct DiffResult {
    float       max_tile = 0.0f;  // plus forte différence moyenne d'une tuile (échelle 0-255)
    float       mean     = 0.0f;
    CutStatsRaw cut;              // sommes de cs_cut_stats (niveau le plus grossier), voir scene_cut.hpp
};

// Noyaux compute : pyramide, détection de doublons, estimation de mouvement.
class MotionEngine {
public:
    void init(D3DContext& d3d);
    void build_pyramid(LumaPyramid& pyr, ID3D11ShaderResourceView* rgb, Size2D src);
    // Masque de bords dilaté (Sobel + max 3x3) du niveau 0, pour la protection du HUD dans ps_synth.
    void build_edges(const LumaPyramid& pyr, GpuTexture& out);
    // Lecture CPU immédiate (attend le GPU, ~0.1 ms de travail).
    DiffResult measure_difference(const LumaPyramid& a, const LumaPyramid& b);
    // Flow X -> Y en UV normalisés sur la grille de blocs 8x8 du niveau 0 (R32G32F).
    // `temporal` : flow de la paire précédente (prédicteur), multiplié par temporal_sign.
    void estimate_flow(const LumaPyramid& x, const LumaPyramid& y, ID3D11ShaderResourceView* temporal,
                       float temporal_sign, GpuTexture& out);

private:
    D3DContext*                         d3d_ = nullptr;
    ComPtr<ID3D11ComputeShader>         cs_luma_rgb_, cs_luma_down_, cs_diff_, cs_cut_, cs_motion_, cs_propagate_,
                                        cs_median_, cs_sobel_, cs_dilate_;
    GpuTexture                          edge_tmp_;
    ComPtr<ID3D11Buffer>                diff_buf_, diff_staging_;
    ComPtr<ID3D11UnorderedAccessView>   diff_uav_;
    std::vector<GpuTexture>             level_flow_, level_tmp_;
};
