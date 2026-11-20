#pragma once
#include "common.hpp"

enum class UpscaleMode : int {
    Off     = 0,  // étirement bilinéaire
    Bicubic = 1,  // Catmull-Rom + netteté anti-halo (shader, tous GPU)
    Vsr     = 2,  // NVIDIA RTX Video Super Resolution via le VideoProcessor D3D11
    Nvvfx   = 3,  // VSR du NVIDIA Video Effects SDK (option RPENHANCE_NVVFX), repli sur Vsr
};

enum class InterpMode : int {
    Flow  = 0,  // interpolation maison (flow par blocs + synthèse)
    Nvvfx = 1,  // Video Frame Generation du NVIDIA Video Effects SDK (option RPENHANCE_NVVFX), repli sur Flow
};

struct Config {
    std::wstring window;          // sous-chaîne du titre ; vide = auto ("PS Remote Play", "chiaki")
    bool  list_windows  = false;
    bool  windowed      = false;  // fenêtre classique au lieu de l'overlay plein écran
    bool  aspect_crop   = true;   // recadre la zone client en 16:9 (enlève les bandes noires)
    bool  test_pattern  = false;  // source synthétique 30 fps dans un flux 60 fps
    bool  selftest      = false;  // test qualité/perf sans fenêtre, puis quitte
    bool  allow_tearing = true;   // VRR/tearing quand la frame generation est coupée
    bool  debug_layer   = false;
    bool  show_stats    = true;
    bool  help          = false;
    int   monitor       = -1;     // -1 = moniteur de la fenêtre capturée

    UpscaleMode upscale = UpscaleMode::Vsr;
    InterpMode  interp  = InterpMode::Flow;
    bool  frame_gen     = true;
    std::wstring nvvfx_dir;       // racine du NVIDIA VFX SDK Core ; vide = recherche automatique
    std::wstring nvvfx_models;    // dossier des modèles (NVVFX_MODEL_DIRECTORY) ; vide = défaut du SDK
    int   vfg_mode      = 1;      // VFG : 0 low, 1 medium, 2 high
    int   nvvfx_vsr_mode = 3;     // VSR du SDK : 0..4 (bicubic, low, medium, high, ultra)
    float sharpness     = 0.35f;
    float dup_threshold = 1.2f;   // différence moyenne max d'une tuile (0-255) sous laquelle une frame est un doublon
    bool  adaptive_dedupe = true; // le seuil suit le bruit de fond (dup_threshold = base) + verrou de rythme 2/3
    float delay_scale   = 1.15f;
    float flow_scale    = 1.0f;   // 0.5..1 : résolution d'estimation du flow (synthèse en pleine résolution)
    bool  simple_cuts   = false;
    bool  hud_protect   = true;
    bool  capturable    = false;  // l'overlay peut être capturé (OBS, partage d'écran) ; défaut : exclu   // protection du HUD/texte fixe (masque de bords) dans la synthèse  // coupures : seulement la différence moyenne (pas corrélation/histogramme)  // marge du retard d'interpolation (x intervalle entre frames uniques)
};

Config parse_command_line();  // lève std::invalid_argument
const wchar_t* usage_text();
