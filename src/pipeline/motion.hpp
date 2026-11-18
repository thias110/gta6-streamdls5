#pragma once
#include "../gpu/d3d_context.hpp"

// Pyramide de luminance R16F ; niveau 0 = demi-résolution de la capture.
struct LumaPyramid {
    static constexpr int kMaxLevels = 5;
    std::vector<GpuTexture> levels;
    Size2D                  source{};

    void ensure(ID3D11Device* dev, Size2D src);
    bool valid() const { return !levels.empty(); }
};

struct DiffResult {
    float max_tile = 0.0f;  // plus forte différence moyenne d'une tuile (échelle 0-255)
    float mean     = 0.0f;
};

// Noyaux compute : pyramide, détection de doublons, estimation de mouvement.
class MotionEngine {
public:
    void init(D3DContext& d3d);
    void build_pyramid(LumaPyramid& pyr, ID3D11ShaderResourceView* rgb, Size2D src);
    // Lecture CPU immédiate (attend le GPU, ~0.1 ms de travail).
    DiffResult measure_difference(const LumaPyramid& a, const LumaPyramid& b);
    // Flow X -> Y en UV normalisés sur la grille de blocs 8x8 du niveau 0 (R32G32F).
    // `temporal` : flow de la paire précédente (prédicteur), multiplié par temporal_sign.
    void estimate_flow(const LumaPyramid& x, const LumaPyramid& y, ID3D11ShaderResourceView* temporal,
                       float temporal_sign, GpuTexture& out);

private:
    D3DContext*                         d3d_ = nullptr;
    ComPtr<ID3D11ComputeShader>         cs_luma_rgb_, cs_luma_down_, cs_diff_, cs_motion_, cs_propagate_, cs_median_;
    ComPtr<ID3D11Buffer>                diff_buf_, diff_staging_;
    ComPtr<ID3D11UnorderedAccessView>   diff_uav_;
    std::vector<GpuTexture>             level_flow_, level_tmp_;
};
