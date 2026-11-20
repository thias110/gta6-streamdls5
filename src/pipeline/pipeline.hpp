#pragma once
#include "../capture/frame_source.hpp"
#include "../gpu/profiler.hpp"
#include "../stats/frame_stats.hpp"
#include "compositor.hpp"
#include "dedupe.hpp"
#include "motion.hpp"
#include "upscaler.hpp"

struct PipelineSettings {
    UpscaleMode upscale       = UpscaleMode::Vsr;
    float       sharpness     = 0.35f;
    bool        frame_gen     = true;
    bool        dedupe        = true;
    float       dup_threshold = 1.2f;   // seuil de base (fixe si adaptive_dedupe est faux)
    bool        adaptive_dedupe = true; // seuil qui suit le bruit de fond + verrou de rythme 2/3
    float       delay_scale   = 1.15f;
    // Changement de plan (différence moyenne > cut.mean_max, ou corrélation des blocs / histogrammes) :
    // pas de flow, et HOLD : la frame précédente reste affichée jusqu'à l'instant de la nouvelle.
    CutParams   cut;
    bool        compare       = false;  // moitié gauche = capture brute
    bool        flow_viz      = false;
    SynthParams synth;
};

struct PipelineInfo {
    Size2D         capture{}, output{};
    Upscaler::Path upscale_path  = Upscaler::Path::Bilinear;
    const char*    vsr_status    = "";
    float          last_metric   = -1.0f;
    float          dup_threshold = 0.0f;  // seuil de doublon effectivement appliqué
    int            cadence_period = 0;    // rythme verrouillé (1 nouvelle image toutes les N captures), 0 = libre
    float          interval_ms   = 0.0f;  // intervalle moyen entre frames uniques
    float          jitter_ms     = 0.0f;
    float          delay_ms      = 0.0f;  // retard d'affichage imposé par l'interpolation
    float          arrival_ms    = 0.0f;  // retard moyen d'arrivée des captures (horodatage -> soumission)
    uint64_t       scene_cuts    = 0;
    float          cut_corr      = 1.0f;  // dernière corrélation des blocs (-1..1)
    float          cut_hist      = 0.0f;  // dernière distance d'histogrammes (0..1)
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

    // `arrival` : instant (now_seconds) où la capture a été récupérée ; sert à mesurer son retard.
    bool submit(const SourceFrame& f, double arrival);  // true si la frame est nouvelle (pas un doublon)
    // `display_time` : instant prévu d'affichage (vblank) de l'image rendue ; = now si inconnu.
    bool render(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, double now, double display_time);  // true si frame générée
    void reset_history();

    PipelineSettings    settings;
    const PipelineInfo& info() const { return info_; }
    Compositor&         compositor() { return comp_; }

private:
    // Historique : l'interpolation a besoin de la paire qui encadre (affichage - retard) ; avec N frames
    // on couvre ~(N - 2) intervalles de retard, d'où la borne appliquée dans render().
    static constexpr size_t kRing = 6;

    struct UniqueFrame {
        bool        valid = false;
        uint64_t    id = 0, prev_id = 0;
        double      time = 0.0;
        LumaPyramid pyr;
        GpuTexture  raw, up, flow_fwd, flow_bwd;  // flow_fwd : précédente -> celle-ci
        GpuTexture  edges;                        // masque de bords (protection du HUD)
        bool        has_edges = false;
        bool        has_flow = false;
        bool        cut_before = false;  // changement de plan entre la précédente et celle-ci : HOLD
        bool        has_raw  = false;  // copie brute (mode comparaison uniquement)
    };
    UniqueFrame* newest();
    UniqueFrame& acquire_slot();

    D3DContext&          d3d_;
    GpuProfiler&         prof_;
    FrameStatsCollector& stats_;
    Compositor           comp_;
    MotionEngine         motion_;
    DupClassifier        dup_;
    Upscaler             upscaler_;

    std::array<UniqueFrame, kRing> ring_;
    LumaPyramid                scratch_;
    Size2D                     capture_{}, output_{};
    uint64_t                   next_id_ = 1;
    double                     interval_ = 0.0, jitter_ = 0.0;
    double                     lag_ = 0.0, lag_jitter_ = 0.0;  // retard d'arrivée des captures
    double                     lead_ = -1.0;                   // affichage prévu - instant de rendu
    PipelineInfo               info_;
};
