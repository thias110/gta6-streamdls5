#include "app.hpp"
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
