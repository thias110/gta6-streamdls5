#include "app.hpp"

#include "capture/mf_capture.hpp"
#include "capture/test_source.hpp"
#include "capture/wgc_capture.hpp"

#include "imgui.h"
#include "imgui_impl_win32.h"

#include <cmath>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
enum HotkeyId { kHkStats = 1, kHkUpscale, kHkFrameGen, kHkCompare, kHkFlow, kHkQuit };

struct MonitorEnum {
    std::vector<HMONITOR> list;
};
BOOL CALLBACK enum_monitor(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    reinterpret_cast<MonitorEnum*>(lp)->list.push_back(m);
    return TRUE;
}
}  // namespace

App::App(const Config& cfg) : cfg_(cfg) {}

App::~App() {
    audio_.stop();
    if (hotkeys_) {
        for (int id = kHkStats; id <= kHkQuit; ++id) UnregisterHotKey(hwnd_, id);
    }
    source_.reset();
    if (hwnd_) DestroyWindow(hwnd_);
}

bool App::open_source() {
    if (cfg_.hdmi) {
        auto cap = std::make_unique<MfCaptureSource>(d3d_);
        if (!cap->start(cfg_.device, cfg_.mode)) {
            std::wstring msg = L"Carte de capture introuvable";
            msg += cfg_.device.empty() ? L"." : L" : \"" + cfg_.device + L"\".";
            msg += L"\n\nBranche-la (USB 3), vérifie qu'elle apparaît comme périphérique vidéo, et que la PS5 a "
                   L"HDCP désactivé (Paramètres > Système > HDMI).\nPériphériques vidéo vus :\n";
            for (const auto& n : list_capture_devices()) msg += L"  • " + n + L"\n";
            log_msg("%s", narrow(msg).c_str());
            MessageBoxW(nullptr, msg.c_str(), L"rpenhance", MB_OK | MB_ICONWARNING);
            return false;
        }
        source_name_ = cap->name();
        source_      = std::move(cap);
        return true;
    }
    if (cfg_.test_pattern) {
        source_      = std::make_unique<TestSource>(d3d_, pipe_->compositor());
        source_name_ = source_->name();
        return true;
    }

    std::wstring title;
    target_ = find_target_window(cfg_.window, &title);
    if (!target_) {
        std::wstring msg = L"Fenêtre introuvable";
        msg += cfg_.window.empty() ? L" (ni \"PS Remote Play\" ni \"chiaki\")." : L" : \"" + cfg_.window + L"\".";
        msg += L"\n\nLance la remote play puis relance rpenhance, ou passe une partie du titre en argument.\n"
               L"Fenêtres visibles :\n";
        int n = 0;
        for (const auto& w : list_capturable_windows()) {
            if (++n > 20) break;
            msg += L"  • " + w.title + L"  [" + w.process + L"]\n";
        }
        log_msg("%s", narrow(msg).c_str());
        MessageBoxW(nullptr, msg.c_str(), L"rpenhance", MB_OK | MB_ICONWARNING);
        return false;
    }
    auto wgc = std::make_unique<WgcSource>(d3d_, cfg_.aspect_crop);
    if (!wgc->start(target_)) return false;
    source_name_ = narrow(title);
    source_      = std::move(wgc);
    return true;
}

void App::create_window() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"rpenhance_window";
    RegisterClassExW(&wc);

    if (cfg_.monitor >= 0) {
        MonitorEnum e;
        EnumDisplayMonitors(nullptr, nullptr, enum_monitor, reinterpret_cast<LPARAM>(&e));
        if (size_t(cfg_.monitor) < e.list.size()) monitor_ = e.list[size_t(cfg_.monitor)];
    }
    if (!monitor_) monitor_ = MonitorFromWindow(target_, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(monitor_, &mi);

    if (cfg_.windowed) {
        RECT r{0, 0, 1600, 900};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"rpenhance", WS_OVERLAPPEDWINDOW, mi.rcWork.left + 40,
                                mi.rcWork.top + 40, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, this);
        ShowWindow(hwnd_, SW_SHOW);
    } else if (passive_) {
        // Overlay plein écran au-dessus de la remote play, qui garde le focus (et donc la manette).
        const RECT& m = mi.rcMonitor;
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"rpenhance",
                                WS_POPUP, m.left, m.top, m.right - m.left, m.bottom - m.top, nullptr, nullptr, inst,
                                this);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    } else {
        // Carte HDMI : la manette est sur la PS5, la fenêtre peut prendre le focus (plein écran sans bordure).
        const RECT& m = mi.rcMonitor;
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"rpenhance", WS_POPUP, m.left, m.top, m.right - m.left,
                                m.bottom - m.top, nullptr, nullptr, inst, this);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }
    if (!hwnd_) throw_hr(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowEx");
    refresh_hz_ = query_refresh_rate();
}

double App::query_refresh_rate() const {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR mon = hwnd_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY) : monitor_;
    if (!GetMonitorInfoW(mon, &mi)) return 60.0;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return double(dm.dmDisplayFrequency);
    return 60.0;
}

Size2D App::client_size() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    return {uint32_t(std::max(0L, rc.right - rc.left)), uint32_t(std::max(0L, rc.bottom - rc.top))};
}

// Plus grand rectangle au format de la capture, centré dans la fenêtre (bandes noires sinon).
D3D11_VIEWPORT App::layout(Size2D client) const {
    const Size2D cap = pipe_->info().capture;
    const double aspect = cap.empty() ? 16.0 / 9.0 : double(cap.width) / double(cap.height);
    double w = client.width, h = client.height;
    if (w / h > aspect) w = std::round(h * aspect);
    else h = std::round(w / aspect);
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = float(std::floor((double(client.width) - w) * 0.5));
    vp.TopLeftY = float(std::floor((double(client.height) - h) * 0.5));
    vp.Width    = float(w);
    vp.Height   = float(h);
    vp.MaxDepth = 1.0f;
    return vp;
}

void App::on_hotkey(int id) {
    auto& s = pipe_->settings;
    switch (id) {
    case kHkStats: overlay_.visible = !overlay_.visible; break;
    case kHkUpscale: s.upscale = UpscaleMode((int(s.upscale) + 1) % 3); break;
    case kHkFrameGen: s.frame_gen = !s.frame_gen; break;
    case kHkCompare: s.compare = !s.compare; break;
    case kHkFlow: s.flow_viz = !s.flow_viz; break;
    case kHkQuit: quit_ = true; break;
    default: break;
    }
}

LRESULT CALLBACK App::wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return app ? app->on_message(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT App::on_message(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (overlay_.initialized() && ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
    switch (m) {
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) resize_pending_ = true;
        return 0;
    case WM_HOTKEY: on_hotkey(int(w)); return 0;
    case WM_MOUSEACTIVATE:
        if (passive_) return MA_NOACTIVATE;
        break;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) quit_ = true;
        return 0;
    case WM_CLOSE: quit_ = true; return 0;
    case WM_DISPLAYCHANGE: refresh_hz_ = query_refresh_rate(); break;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

int App::run() {
    d3d_.initialize(cfg_.debug_layer);
    prof_.init(d3d_.device());
    pipe_ = std::make_unique<Pipeline>(d3d_, prof_, stats_);
    pipe_->init();
    auto& s         = pipe_->settings;
    s.upscale       = cfg_.upscale;
    s.sharpness     = cfg_.sharpness;
    s.frame_gen     = cfg_.frame_gen;
    s.dup_threshold = cfg_.dup_threshold;
    s.delay_scale   = cfg_.delay_scale;

    passive_ = !cfg_.windowed && !cfg_.hdmi;
    if (!open_source()) return 1;
    if (cfg_.hdmi && cfg_.audio) {
        try {
            audio_.start(cfg_.audio_device, source_name_, cfg_.audio_volume);
        } catch (const std::exception& e) {
            log_msg("Son : relais impossible (%s) ; l'image continue sans le son.", e.what());
        }
    }
    create_window();
    sc_.create(d3d_, hwnd_, client_size());
    overlay_.init(hwnd_, d3d_, float(GetDpiForWindow(hwnd_)) / 96.0f);
    overlay_.visible = cfg_.show_stats;

    const UINT mods = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
    hotkeys_ = true;
    const std::pair<int, UINT> keys[] = {{kHkStats, 'S'}, {kHkUpscale, 'U'}, {kHkFrameGen, 'F'},
                                         {kHkCompare, 'C'}, {kHkFlow, 'V'},   {kHkQuit, 'Q'}};
    for (auto [id, vk] : keys) {
        if (!RegisterHotKey(hwnd_, id, mods, vk)) log_msg("Raccourci Ctrl+Alt+%c déjà pris par une autre appli", char(vk));
    }
    if (passive_ && target_) {
        if (!SetForegroundWindow(target_))
            log_msg("Impossible de redonner le focus à la remote play : fais Alt+Tab dessus.");
    }

    OverlayModel model;
    model.source_name  = source_name_;
    model.adapter      = d3d_.adapter_name();
    model.overlay_mode = passive_;
    model.hdmi         = cfg_.hdmi;
    model.audio        = audio_.running() ? &audio_ : nullptr;
    model.audio_volume = cfg_.audio_volume;
    log_msg("Démarré : %s, overlay %s, %.0f Hz", source_name_.c_str(), passive_ ? "oui" : "non", refresh_hz_);

    double last_log = now_seconds();
    while (!quit_ && !overlay_.quit_requested) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit_ = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit_) break;
        if (!source_->alive()) {
            log_msg(cfg_.hdmi ? "La carte de capture a cessé d'envoyer des images." : "La fenêtre capturée a été fermée.");
            break;
        }
        if (resize_pending_) {
            sc_.resize(client_size());
            resize_pending_ = false;
        }

        const bool fg = pipe_->settings.frame_gen;
        if (fg) {
            // Interpolation : on rend une image par rafraîchissement de l'écran.
            WaitForSingleObjectEx(sc_.waitable(), 100, TRUE);
        } else {
            // Passthrough : on présente dès qu'une capture arrive (VRR), sinon ~30 Hz pour l'UI.
            HANDLE h       = source_->wait_handle();
            DWORD  timeout = 33;
            if (!h && source_->next_due() > 0.0)
                timeout = DWORD(std::clamp((source_->next_due() - now_seconds()) * 1000.0, 0.0, 33.0));
            MsgWaitForMultipleObjects(h ? 1 : 0, h ? &h : nullptr, FALSE, timeout, QS_ALLINPUT);
        }

        auto* ctx = d3d_.ctx();
        prof_.begin_frame(ctx);

        SourceFrame frame;
        if (source_->poll(frame)) pipe_->submit(frame);

        const Size2D         client = sc_.size();
        const D3D11_VIEWPORT vp     = layout(client);
        pipe_->set_output_size({uint32_t(vp.Width), uint32_t(vp.Height)});

        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx->ClearRenderTargetView(sc_.rtv(), black);
        const double now       = now_seconds();
        const bool   generated = pipe_->render(sc_.rtv(), vp, now);

        model.refresh_hz = refresh_hz_;
        overlay_.render(sc_.rtv(), model, *pipe_, stats_, prof_, now);

        sc_.present(fg, cfg_.allow_tearing);
        prof_.end_frame(ctx);
        stats_.on_present(now_seconds(), generated);

        if (now - last_log >= 5.0) {
            last_log = now;
            const auto  st   = stats_.snapshot(now);
            const auto& info = pipe_->info();
            log_msg("capturé %.0f fps | unique %.0f fps | affiché %.0f fps (%.0f%% générées) | %ux%u -> %ux%u (%s) | "
                    "retard FG %.1f ms | GPU flow %.2f ms",
                    st.captured_fps, st.unique_fps, st.present_fps, st.generated_pct, info.capture.width,
                    info.capture.height, info.output.width, info.output.height,
                    Upscaler::path_name(info.upscale_path), info.delay_ms, prof_.ms(GpuSection::Flow));
            if (audio_.running())
                log_msg("son : %.1f ms en tampon | %llu trames relayées, %llu jetées", audio_.buffered_ms(),
                        static_cast<unsigned long long>(audio_.frames_relayed()),
                        static_cast<unsigned long long>(audio_.frames_dropped()));
        }
    }
    return 0;
}
