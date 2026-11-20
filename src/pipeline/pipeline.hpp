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
    InterpMode  interp        = InterpMode::Flow;  // Nvvfx : VFG du SDK NVIDIA, repli sur le flow maison
    float       sharpness     = 0.35f;
    bool        frame_gen     = true;
    bool        dedupe        = true;
    float       dup_threshold = 1.2f;   // seuil de base (fixe si adaptive_dedupe est faux)
    bool        adaptive_dedupe = true; // seuil qui suit le bruit de fond + verrou de rythme 2/3
    float       delay_scale   = 1.15f;
    float       flow_scale    = 1.0f;   // 0.5..1 : résolution d'estimation du flow (synthèse toujours pleine)
    // Changement de plan (différence moyenne > cut.mean_max, ou corrélation des blocs / histogrammes) :
    // pas de flow, et HOLD : la frame précédente reste affichée jusqu'à l'instant de la nouvelle.
    CutParams   cut;
    // --tap-trust-flags : croire les métadonnées de la source (frame tap) au lieu de les deviner. Désactivé :
    // le pipeline ignore SourceFrame::meta (comportement inchangé).
    bool        trust_source_flags = false;
    bool        compare       = false;  // moitié gauche = capture brute
    bool        flow_viz      = false;
    SynthParams synth;
};

struct PipelineInfo {
    Size2D         capture{}, output{};
    Upscaler::Path upscale_path  = Upscaler::Path::Bilinear;
    const char*    vsr_status    = "";
    bool           interp_nvvfx  = false;  // dernière image générée par VFG (SDK NVIDIA)
    const char*    vfg_status    = "";
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
    uint64_t       blocking_reads = 0;
    Size2D         flow_res{};          // résolution du niveau 0 de la pyramide (estimation du flow)  // lectures de mesure qui ont dû attendre le GPU
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
    // La décision doublon / nouvelle est différée : la mesure GPU est lue au prochain render() qui en
    // a besoin ou au prochain submit(). Renvoie true seulement si la frame a été retenue tout de suite
    // (pas de mesure nécessaire).
    bool submit(const SourceFrame& f, double arrival);
    // `display_time` : instant prévu d'affichage (vblank) de l'image rendue ; = now si inconnu.
    bool render(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, double now, double display_time);  // true si frame générée
    void reset_history();

    // Options du backend NVIDIA VFX (dossier du SDK, modes) ; à appeler avant le premier rendu.
    void configure_nvvfx(const NvvfxOptions& o) { nvvfx_.set_options(o); }

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
        // prev_id : id de la frame unique précédente du même plan (paire interpolable), 0 sinon.
        GpuTexture  edges;                        // masque de bords (protection du HUD)
        bool        has_edges = false;
        bool        has_flow = false;
        bool        cut_before = false;  // changement de plan entre la précédente et celle-ci : HOLD
        bool        damaged  = false;  // source : frame perdue avant celle-ci ou tronquée (trust_source_flags)
        bool        has_raw  = false;  // copie brute (mode comparaison uniquement)
    };
    UniqueFrame* newest();
    UniqueFrame& acquire_slot();
    // Lit la mesure de la capture en attente (sans attendre le GPU si wait = false) et la traite.
    void resolve_pending(bool wait);
    // Ce que la source affirme de la frame (seulement avec settings.trust_source_flags).
    struct SourceHint {
        bool fresh   = false;  // nouveau contenu : unique sans passer par DupClassifier
        bool damaged = false;  // perte avant / frame tronquée : ni interpolation vers elle ni depuis elle
    };
    // Décision doublon / coupure puis ajout à l'historique (upscale, flow). d = nullptr : pas de mesure.
    bool accept(const GpuTexture& src, double time, double arrival, UniqueFrame* last, const DiffResult* d,
                SourceHint hint);

    struct Pending {
        bool         active  = false;
        double       time    = 0.0, arrival = 0.0;
        UniqueFrame* ref     = nullptr;  // frame de référence de la mesure
        uint64_t     ref_id  = 0;
        SourceHint   hint;
    };

    D3DContext&          d3d_;
    GpuProfiler&         prof_;
    FrameStatsCollector& stats_;
    Compositor           comp_;
    MotionEngine         motion_;
    DupClassifier        dup_;
    NvvfxBackend         nvvfx_;     // optionnel (RPENHANCE_NVVFX) ; partagé avec upscaler_
    Upscaler             upscaler_;
    GpuTexture           vfg_out_;   // image générée par VFG, en taille de sortie

    std::array<UniqueFrame, kRing> ring_;
    LumaPyramid                scratch_;      // pyramide de la capture en cours / en attente
    GpuTexture                 pending_src_;  // copie de la capture en attente de sa mesure
    Pending                    pending_;
    Size2D                     capture_{}, output_{};
    float                      pyr_scale_ = 1.0f;  // échelle des pyramides de l'historique
    uint64_t                   next_id_ = 1;
    double                     interval_ = 0.0, jitter_ = 0.0;
    double                     lag_ = 0.0, lag_jitter_ = 0.0;  // retard d'arrivée des captures
    double                     lead_ = -1.0;                   // affichage prévu - instant de rendu
    PipelineInfo               info_;
};
