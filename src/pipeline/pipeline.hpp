#pragma once
#include "../capture/frame_source.hpp"
#include "../gpu/profiler.hpp"
#include "../stats/frame_stats.hpp"
#include "compositor.hpp"
#include "motion.hpp"
#include "upscaler.hpp"

struct PipelineSettings {
    UpscaleMode upscale       = UpscaleMode::Vsr;
    float       sharpness     = 0.35f;
    bool        frame_gen     = true;
    bool        dedupe        = true;
    float       dup_threshold = 1.2f;
    float       delay_scale   = 1.15f;
    bool        compare       = false;  // moitié gauche = capture brute
    bool        flow_viz      = false;
    SynthParams synth;
};

struct PipelineInfo {
    Size2D         capture{}, output{};
    Upscaler::Path upscale_path  = Upscaler::Path::Bilinear;
    const char*    vsr_status    = "";
    float          last_metric   = -1.0f;
    float          interval_ms   = 0.0f;  // intervalle moyen entre frames uniques
    float          jitter_ms     = 0.0f;
    float          delay_ms      = 0.0f;  // retard d'affichage imposé par l'interpolation
    float          last_alpha    = 0.0f;
    bool           interpolating = false;
    uint64_t       unique_frames = 0;
};

// capture -> dédoublonnage -> upscale -> flow (paire précédente) ; puis à chaque rafraîchissement
// de l'écran, synthèse de l'image correspondant à l'instant (maintenant - retard).
class Pipeline {
public:
    Pipeline(D3DContext& d3d, GpuProfiler& prof, FrameStatsCollector& stats);
    void init();

    void   set_output_size(Size2D s);
    Size2D output_size() const { return output_; }

    bool submit(const SourceFrame& f);  // true si la frame est nouvelle (pas un doublon)
    bool render(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, double now);  // true si frame générée
    void reset_history();

    PipelineSettings    settings;
    const PipelineInfo& info() const { return info_; }
    Compositor&         compositor() { return comp_; }

private:
    struct UniqueFrame {
        bool        valid = false;
        uint64_t    id = 0, prev_id = 0;
        double      time = 0.0;
        LumaPyramid pyr;
        GpuTexture  raw, up, flow_fwd, flow_bwd;  // flow_fwd : précédente -> celle-ci
        bool        has_flow = false;
    };
    UniqueFrame* newest();
    UniqueFrame& acquire_slot();

    D3DContext&          d3d_;
    GpuProfiler&         prof_;
    FrameStatsCollector& stats_;
    Compositor           comp_;
    MotionEngine         motion_;
    Upscaler             upscaler_;

    std::array<UniqueFrame, 4> ring_;
    LumaPyramid                scratch_;
    Size2D                     capture_{}, output_{};
    uint64_t                   next_id_ = 1;
    double                     interval_ = 0.0, jitter_ = 0.0;
    PipelineInfo               info_;
};
