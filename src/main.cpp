#include "app.hpp"
#include "capture/audio_passthrough.hpp"
#include "capture/mf_capture.hpp"
#include "capture/wgc_capture.hpp"
#include "config.hpp"
#include "selftest.hpp"

#include <winrt/base.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    log_init((exe_directory() + L"rpenhance.log").c_str());

    Config cfg;
    try {
        cfg = parse_command_line();
    } catch (const std::exception& e) {
        std::wstring msg = widen(e.what()) + L"\n\n" + usage_text();
        log_msg("%s", narrow(msg).c_str());
        MessageBoxW(nullptr, msg.c_str(), L"rpenhance", MB_OK | MB_ICONERROR);
        return 2;
    }
    if (cfg.help) {
        log_msg("%s", narrow(usage_text()).c_str());
        MessageBoxW(nullptr, usage_text(), L"rpenhance", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    if (cfg.list_devices) {
        // Chaque partie a son propre try : un échec de l'énumération vidéo n'empêche pas de lister l'audio.
        std::wstring text = L"Cartes de capture / périphériques vidéo :\n\n";
        try {
            auto devices = list_capture_devices();
            if (devices.empty()) {
                text += L"(aucun)\n";
                log_msg("(aucun périphérique vidéo)");
            }
            for (const auto& d : devices) {
                text += d + L"\n";
                log_msg("vidéo : %s", narrow(d).c_str());
            }
        } catch (const std::exception& e) {
            text += L"(énumération vidéo impossible : " + widen(e.what()) + L")\n";
            log_msg("énumération vidéo impossible : %s", e.what());
        }
        if (cfg.capture_source()) {  // --device / --mode / --source capture : modes natifs de la carte choisie
            try {
                std::wstring             dev;
                std::vector<std::string> modes;
                if (list_capture_modes(cfg.device, dev, modes)) {
                    text += L"\nModes natifs de \"" + dev + L"\" (pour --mode) :\n\n";
                    log_msg("modes natifs de %s :", narrow(dev).c_str());
                    for (const auto& md : modes) {
                        text += widen(md) + L"\n";
                        log_msg("  %s", md.c_str());
                    }
                } else {
                    text += L"\nAucun périphérique ne correspond à --device.\n";
                }
            } catch (const std::exception& e) {
                text += L"\n(modes illisibles : " + widen(e.what()) + L")\n";
                log_msg("modes illisibles : %s", e.what());
            }
        }
        text += L"\nEntrées audio :\n\n";
        try {
            for (const auto& a : list_audio_inputs()) {
                text += a + L"\n";
                log_msg("audio : %s", narrow(a).c_str());
            }
        } catch (const std::exception& e) {
            text += L"(énumération audio impossible : " + widen(e.what()) + L")\n";
            log_msg("énumération audio impossible : %s", e.what());
        }
        MessageBoxW(nullptr, text.c_str(), L"rpenhance --list-devices", MB_OK);
        return 0;
    }

    if (cfg.list_windows) {
        std::wstring text = L"Fenêtres capturables :\n\n";
        for (const auto& w : list_capturable_windows()) {
            text += w.title + L"   [" + w.process + L"]\n";
            log_msg("%s   [%s]", narrow(w.title).c_str(), narrow(w.process).c_str());
        }
        MessageBoxW(nullptr, text.c_str(), L"rpenhance --list", MB_OK);
        return 0;
    }

    try {
        if (cfg.selftest_tap) return run_selftest_tap(cfg);
        if (cfg.selftest) return run_selftest(cfg);
        App app(cfg);
        return app.run();
    } catch (const winrt::hresult_error& e) {
        std::string msg = "Erreur WinRT 0x" + std::to_string(uint32_t(e.code())) + " : " + winrt::to_string(e.message());
        log_msg("%s", msg.c_str());
        if (!cfg.selftest && !cfg.selftest_tap)
            MessageBoxA(nullptr, msg.c_str(), "rpenhance - erreur fatale", MB_OK | MB_ICONERROR);
    } catch (const std::exception& e) {
        log_msg("Erreur fatale : %s", e.what());
        if (!cfg.selftest && !cfg.selftest_tap) MessageBoxA(nullptr, e.what(), "rpenhance - erreur fatale", MB_OK | MB_ICONERROR);
    }
    return -1;
}
