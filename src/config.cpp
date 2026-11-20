#include "config.hpp"
#include "capture/capture_logic.hpp"

#include <shellapi.h>

const wchar_t* usage_text() {
    return LR"(rpenhance [titre] [options]

  titre / -w, --window <txt>  sous-chaîne du titre de la fenêtre à capturer
                              (défaut : "PS Remote Play" puis "chiaki")
  --list                      liste les fenêtres capturables

  Carte de capture HDMI (--source capture, PS5 branchée par câble) :
  --source capture            lit une carte de capture via Media Foundation
  --hdmi                      alias de --source capture
  --device <txt>              sous-chaîne du nom de la carte (implique --source
                              capture ; défaut : détection Elgato, AVerMedia,
                              « capture », « HDMI », « USB Video »..., sinon la 1re)
  --mode <WxH[@fps]>          mode natif, ex. 1920x1080@60 ou 1280x720 (implique
                              --source capture ; défaut : >= 60 fps, puis la plus
                              grande résolution <= 4K, puis non compressé)
  --list-devices              liste les cartes de capture et les entrées audio ;
                              avec --device ou --source capture : aussi les modes
                              natifs de la carte choisie
  --audio-device <txt>        entrée audio de la carte (défaut : auto)
  --audio-volume <0..1>       volume du son relayé (défaut : 1)
  --no-audio                  ne relaie pas le son de la carte
  --audio-delay auto|<ms>|off retard du son relayé (défaut : auto = le retard que
                              l'interpolation ajoute à l'image, pour que le son
                              n'arrive pas en avance ; <ms> fixe de 0 à 250 ; off = 0)

  --windowed                  fenêtre classique au lieu de l'overlay plein écran
  --monitor <n>               moniteur de l'overlay (0, 1, ...)
  --capturable                l'overlay reste visible des captures d'écran
                              (par défaut il en est exclu)
  --upscale off|bicubic|vsr|nvvfx
                              méthode d'upscale (défaut : vsr) ; nvvfx = VSR du
                              NVIDIA Video Effects SDK (build RPENHANCE_NVVFX),
                              repli sur vsr s'il est indisponible
  --interp flow|nvvfx         interpolation : flow maison (défaut) ou Video Frame
                              Generation du SDK NVIDIA (Ada/Blackwell, pilote
                              >= 570.65), repli sur flow s'il est indisponible
  --nvvfx-dir <dossier>       racine du NVIDIA VFX SDK Core (défaut : NVVFX_SDK_DIR,
                              VFXSDK_ROOT, <exe>\nvvfx-sdk, dossier de compilation)
  --nvvfx-models <dossier>    dossier des modèles du SDK (défaut : celui du SDK)
  --vfg-mode low|medium|high  modèle VFG (défaut : medium)
  --nvvfx-vsr-mode <0..4>     mode VSR du SDK : 0 bicubic .. 4 ultra (défaut : 3)
  --sharpness <0..1>          netteté du mode bicubic (défaut : 0.35)
  --no-fg                     désactive la frame generation
  --dup-threshold <x>         seuil de base de détection des doublons (défaut : 1.2, 0.4 en HDMI)
  --no-adaptive-dedupe        seuil de doublon fixe, sans verrou de rythme
  --no-hud-protect            pas de masque de bords pour garder le HUD fixe
  --simple-cuts               coupures détectées sur la seule différence moyenne
                              (sans corrélation par blocs ni histogramme)
  --delay <x>                 marge du retard d'interpolation (défaut : 1.15)
  --flow-scale <0.5..1>       résolution d'estimation du flow (défaut : 1.0) ; la
                              synthèse reste en pleine résolution
  --no-crop                   ne recadre pas la capture de fenêtre en 16:9 (wgc ;
                              la carte de capture n'est jamais recadrée)
  --no-tearing                pas de tearing/VRR sans frame generation
  --no-stats                  démarre avec l'overlay de stats masqué
  --source wgc|tap|capture    source des frames : capture de fenêtre (wgc, défaut),
                              frame tap (frames décodées NV12/BGRA déposées en mémoire
                              partagée par un autre programme, format SmoovPlay v2) ou
                              carte de capture HDMI (capture, voir plus haut)
  --tap-name <nom>            nom de la mémoire partagée du frame tap
                              (défaut : Local\smoovplay-tap)
  --tap-trust-flags           le pipeline croit les métadonnées du tap : répétition
                              annoncée = doublon sans mesure, frame perdue/tronquée =
                              pas d'interpolation vers ou depuis elle (désactivé par défaut)
  --test-pattern              source synthétique (avec --source capture : profil
                              « câble », répétitions identiques au pixel, sans gigue ;
                              pas besoin de PS5)
  --selftest                  mesure qualité/perf de l'interpolation puis quitte
  --selftest-tap              lit des frames du tap (émises par tap_writer), vérifie
                              leur contenu, puis quitte (code 0 si tout passe)
  --debug                     active la couche de debug D3D11
  --warp                      rendu logiciel D3D11 WARP au lieu du GPU (CI sans
                              GPU, très lent ; échoue tout de suite s'il manque
                              une fonctionnalité D3D11 nécessaire)

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

    bool dup_set = false, source_set = false, capture_implied = false;
    for (size_t i = 0; i < a.size(); ++i) {
        const std::wstring& s = a[i];
        if (s == L"--help" || s == L"-h" || s == L"/?") c.help = true;
        else if (s == L"--list") c.list_windows = true;
        else if (s == L"--hdmi") { c.source = SourceKind::Capture; source_set = true; }
        else if (s == L"--list-devices") c.list_devices = true;
        else if (s == L"--device") { c.device = value(i); capture_implied = true; }
        else if (s == L"--mode") { c.mode = value(i); capture_implied = true; }
        else if (s == L"--no-audio") c.audio = false;
        else if (s == L"--audio-device") c.audio_device = value(i);
        else if (s == L"--audio-volume") c.audio_volume = number(i);
        else if (s == L"--audio-delay") {
            std::string err;
            if (!audio_delay::parse(value(i), c.audio_delay, &err)) throw std::invalid_argument(err);
        }
        else if (s == L"--window" || s == L"-w") c.window = value(i);
        else if (s == L"--windowed") c.windowed = true;
        else if (s == L"--monitor") c.monitor = int(number(i));
        else if (s == L"--no-crop") c.aspect_crop = false;
        else if (s == L"--no-tearing") c.allow_tearing = false;
        else if (s == L"--no-stats") c.show_stats = false;
        else if (s == L"--test-pattern") c.test_pattern = true;
        else if (s == L"--selftest") c.selftest = true;
        else if (s == L"--selftest-tap") c.selftest_tap = true;
        else if (s == L"--tap-name") c.tap_name = value(i);
        else if (s == L"--tap-trust-flags") c.tap_trust_flags = true;
        else if (s == L"--source") {
            auto v = to_lower(value(i));
            if (v == L"wgc") c.source = SourceKind::Wgc;
            else if (v == L"tap") c.source = SourceKind::Tap;
            else if (v == L"capture" || v == L"hdmi") c.source = SourceKind::Capture;
            else throw std::invalid_argument("--source attend wgc, tap ou capture");
            source_set = true;
        }
        else if (s == L"--debug") c.debug_layer = true;
        else if (s == L"--warp") c.warp = true;
        else if (s == L"--no-fg") c.frame_gen = false;
        else if (s == L"--fg") c.frame_gen = true;
        else if (s == L"--sharpness") c.sharpness = number(i);
        else if (s == L"--dup-threshold") { c.dup_threshold = number(i); dup_set = true; }
        else if (s == L"--no-adaptive-dedupe") c.adaptive_dedupe = false;
        else if (s == L"--simple-cuts") c.simple_cuts = true;
        else if (s == L"--no-hud-protect") c.hud_protect = false;
        else if (s == L"--capturable") c.capturable = true;
        else if (s == L"--delay") c.delay_scale = number(i);
        else if (s == L"--flow-scale") c.flow_scale = number(i);
        else if (s == L"--upscale") {
            auto v = to_lower(value(i));
            if (v == L"off") c.upscale = UpscaleMode::Off;
            else if (v == L"bicubic") c.upscale = UpscaleMode::Bicubic;
            else if (v == L"vsr") c.upscale = UpscaleMode::Vsr;
            else if (v == L"nvvfx") c.upscale = UpscaleMode::Nvvfx;
            else throw std::invalid_argument("--upscale attend off, bicubic, vsr ou nvvfx");
        }
        else if (s == L"--interp") {
            auto v = to_lower(value(i));
            if (v == L"flow") c.interp = InterpMode::Flow;
            else if (v == L"nvvfx") c.interp = InterpMode::Nvvfx;
            else throw std::invalid_argument("--interp attend flow ou nvvfx");
        }
        else if (s == L"--nvvfx-dir") c.nvvfx_dir = value(i);
        else if (s == L"--nvvfx-models") c.nvvfx_models = value(i);
        else if (s == L"--nvvfx-vsr-mode") c.nvvfx_vsr_mode = int(number(i));
        else if (s == L"--vfg-mode") {
            auto v = to_lower(value(i));
            if (v == L"low") c.vfg_mode = 0;
            else if (v == L"medium") c.vfg_mode = 1;
            else if (v == L"high") c.vfg_mode = 2;
            else throw std::invalid_argument("--vfg-mode attend low, medium ou high");
        }
        else if (!s.empty() && s[0] == L'-') throw std::invalid_argument("option inconnue : " + narrow(s));
        else c.window = s;
    }

    // HDMI : pas de réencodage, les images répétées sont identiques ; un seuil bas ne rate aucun petit mouvement.
    if (capture_implied) {
        if (source_set && c.source != SourceKind::Capture)
            throw std::invalid_argument("--device et --mode ne servent qu'avec --source capture");
        c.source = SourceKind::Capture;
    }
    {
        capture_logic::ModeRequest req;
        std::string                err;
        if (!capture_logic::parse_mode(narrow(c.mode), req, &err)) throw std::invalid_argument(err);
    }
    if (c.capture_source() && !dup_set) c.dup_threshold = 0.4f;

    c.audio_volume  = std::clamp(c.audio_volume, 0.0f, 1.0f);
    c.sharpness     = std::clamp(c.sharpness, 0.0f, 1.0f);
    c.dup_threshold = std::clamp(c.dup_threshold, 0.0f, 50.0f);
    c.delay_scale   = std::clamp(c.delay_scale, 1.0f, 3.0f);
    c.flow_scale    = std::clamp(c.flow_scale, 0.5f, 1.0f);
    c.nvvfx_vsr_mode = std::clamp(c.nvvfx_vsr_mode, 0, 4);  // modes d'upscale du guide VSR (0..4)
    if (c.tap_name.empty()) throw std::invalid_argument("--tap-name attend un nom non vide");
    return c;
}
