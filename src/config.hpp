#pragma once
#include "common.hpp"

enum class UpscaleMode : int {
    Off     = 0,  // étirement bilinéaire
    Bicubic = 1,  // Catmull-Rom + netteté anti-halo (shader, tous GPU)
    Vsr     = 2,  // NVIDIA RTX Video Super Resolution via le VideoProcessor D3D11
};

struct Config {
    std::wstring window;          // sous-chaîne du titre ; vide = auto ("PS Remote Play", "chiaki")
    bool  list_windows  = false;
    bool  hdmi          = false;  // carte de capture HDMI au lieu de la fenêtre Remote Play
    bool  list_devices  = false;
    std::wstring device;          // sous-chaîne du nom de la carte ; vide = auto
    std::wstring mode;            // "1920x1080@60" ; vide = auto
    bool  audio         = true;   // relais du son de la carte (mode HDMI)
    std::wstring audio_device;    // sous-chaîne du nom de l'entrée audio ; vide = auto
    float audio_volume  = 1.0f;
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
    bool  frame_gen     = true;
    float sharpness     = 0.35f;
    float dup_threshold = 1.2f;   // différence moyenne max d'une tuile (0-255) sous laquelle une frame est un doublon
    float delay_scale   = 1.15f;  // marge du retard d'interpolation (x intervalle entre frames uniques)
};

Config parse_command_line();  // lève std::invalid_argument
const wchar_t* usage_text();
