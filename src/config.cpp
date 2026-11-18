#include "config.hpp"

#include <shellapi.h>

const wchar_t* usage_text() {
    return LR"(rpenhance [titre] [options]

  titre / -w, --window <txt>  sous-chaîne du titre de la fenêtre à capturer
                              (défaut : "PS Remote Play" puis "chiaki")
  --list                      liste les fenêtres capturables

  --hdmi                      PS5 branchée sur une carte de capture HDMI
  --device <txt>              sous-chaîne du nom de la carte (implique --hdmi)
  --mode <WxH@fps>            mode de capture, ex. 1920x1080@60 (défaut : auto)
  --list-devices              liste les cartes de capture et les entrées audio
  --audio-device <txt>        entrée audio de la carte (défaut : auto)
  --audio-volume <0..1>       volume du son relayé (défaut : 1)
  --no-audio                  ne relaie pas le son de la carte

  --windowed                  fenêtre classique au lieu de l'overlay plein écran
  --monitor <n>               moniteur de l'overlay (0, 1, ...)
  --upscale off|bicubic|vsr   méthode d'upscale (défaut : vsr)
  --sharpness <0..1>          netteté du mode bicubic (défaut : 0.35)
  --no-fg                     désactive la frame generation
  --dup-threshold <x>         seuil de détection des doublons (défaut : 1.2, 0.4 en HDMI)
  --delay <x>                 marge du retard d'interpolation (défaut : 1.15)
  --no-crop                   ne recadre pas la capture en 16:9
  --no-tearing                pas de tearing/VRR sans frame generation
  --no-stats                  démarre avec l'overlay de stats masqué
  --test-pattern              source synthétique (pas besoin de PS5)
  --selftest                  mesure qualité/perf de l'interpolation puis quitte
  --debug                     active la couche de debug D3D11

Raccourcis globaux : Ctrl+Alt+S stats, U upscale, F frame gen,
                     C comparaison, V flow, Q quitter)";
}

Config parse_command_line() {
    Config c;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> a;
    for (int i = 1; i < argc; ++i) a.emplace_back(argv[i]);
    LocalFree(argv);

    auto value = [&](size_t& i) -> const std::wstring& {
        if (i + 1 >= a.size()) throw std::invalid_argument("valeur manquante après " + narrow(a[i]));
        return a[++i];
    };
    auto number = [&](size_t& i) -> float {
        const auto& v = value(i);
        try {
            return std::stof(v);
        } catch (...) {
            throw std::invalid_argument("nombre invalide : " + narrow(v));
        }
    };

    bool dup_set = false;
    for (size_t i = 0; i < a.size(); ++i) {
        const std::wstring& s = a[i];
        if (s == L"--help" || s == L"-h" || s == L"/?") c.help = true;
        else if (s == L"--list") c.list_windows = true;
        else if (s == L"--hdmi") c.hdmi = true;
        else if (s == L"--list-devices") c.list_devices = true;
        else if (s == L"--device") { c.device = value(i); c.hdmi = true; }
        else if (s == L"--mode") { c.mode = value(i); c.hdmi = true; }
        else if (s == L"--no-audio") c.audio = false;
        else if (s == L"--audio-device") c.audio_device = value(i);
        else if (s == L"--audio-volume") c.audio_volume = number(i);
        else if (s == L"--window" || s == L"-w") c.window = value(i);
        else if (s == L"--windowed") c.windowed = true;
        else if (s == L"--monitor") c.monitor = int(number(i));
        else if (s == L"--no-crop") c.aspect_crop = false;
        else if (s == L"--no-tearing") c.allow_tearing = false;
        else if (s == L"--no-stats") c.show_stats = false;
        else if (s == L"--test-pattern") c.test_pattern = true;
        else if (s == L"--selftest") c.selftest = true;
        else if (s == L"--debug") c.debug_layer = true;
        else if (s == L"--no-fg") c.frame_gen = false;
        else if (s == L"--fg") c.frame_gen = true;
        else if (s == L"--sharpness") c.sharpness = number(i);
        else if (s == L"--dup-threshold") { c.dup_threshold = number(i); dup_set = true; }
        else if (s == L"--delay") c.delay_scale = number(i);
        else if (s == L"--upscale") {
            auto v = to_lower(value(i));
            if (v == L"off") c.upscale = UpscaleMode::Off;
            else if (v == L"bicubic") c.upscale = UpscaleMode::Bicubic;
            else if (v == L"vsr") c.upscale = UpscaleMode::Vsr;
            else throw std::invalid_argument("--upscale attend off, bicubic ou vsr");
        }
        else if (!s.empty() && s[0] == L'-') throw std::invalid_argument("option inconnue : " + narrow(s));
        else c.window = s;
    }

    // HDMI : pas de réencodage, les images répétées sont identiques ; un seuil bas ne rate aucun petit mouvement.
    if (c.hdmi && !dup_set) c.dup_threshold = 0.4f;

    c.audio_volume  = std::clamp(c.audio_volume, 0.0f, 1.0f);
    c.sharpness     = std::clamp(c.sharpness, 0.0f, 1.0f);
    c.dup_threshold = std::clamp(c.dup_threshold, 0.0f, 50.0f);
    c.delay_scale   = std::clamp(c.delay_scale, 1.0f, 3.0f);
    return c;
}
