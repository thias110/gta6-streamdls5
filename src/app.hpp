#pragma once
#include "capture/frame_source.hpp"
#include "config.hpp"
#include "gpu/profiler.hpp"
#include "overlay/imgui_overlay.hpp"
#include "pipeline/pipeline.hpp"
#include "present/swapchain.hpp"
#include "stats/frame_stats.hpp"

class App {
public:
    explicit App(const Config& cfg);
    ~App();
    int run();

private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT on_message(HWND, UINT, WPARAM, LPARAM);

    bool           open_source();
    void           create_window();
    void           on_hotkey(int id);
    double         query_refresh_rate() const;
    Size2D         client_size() const;
    D3D11_VIEWPORT layout(Size2D client) const;

    Config              cfg_;
    D3DContext          d3d_;
    GpuProfiler         prof_;
    FrameStatsCollector stats_;
    std::unique_ptr<Pipeline>    pipe_;
    std::unique_ptr<FrameSource> source_;
    Swapchain           sc_;
    ImguiOverlay        overlay_;

    HWND        hwnd_   = nullptr;
    HWND        target_ = nullptr;
    HMONITOR    monitor_ = nullptr;
    bool        quit_           = false;
    bool        resize_pending_ = false;
    bool        hotkeys_        = false;
    bool        passive_        = false;  // overlay qui ne prend jamais le focus (mode Remote Play)
    double      refresh_hz_     = 60.0;
    std::string source_name_;
};
