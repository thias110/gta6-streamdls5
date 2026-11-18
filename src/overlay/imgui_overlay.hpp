#pragma once
#include "../gpu/profiler.hpp"
#include "../pipeline/pipeline.hpp"
#include "../stats/frame_stats.hpp"

struct OverlayModel {
    std::string source_name;
    std::string adapter;
    double      refresh_hz   = 60.0;
    bool        overlay_mode = true;
    bool        hdmi         = false;
    class AudioPassthrough* audio = nullptr;
    float       audio_volume = 1.0f;
};

class ImguiOverlay {
public:
    ~ImguiOverlay();
    void init(HWND hwnd, D3DContext& d3d, float dpi_scale);
    bool initialized() const { return init_; }
    void render(ID3D11RenderTargetView* rtv, OverlayModel& m, Pipeline& pipe, const FrameStatsCollector& stats,
                const GpuProfiler& prof, double now);

    bool visible        = true;
    bool quit_requested = false;

private:
    D3DContext* d3d_  = nullptr;
    bool        init_ = false;
};
