#include "imgui_overlay.hpp"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

ImguiOverlay::~ImguiOverlay() {
    if (init_) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

void ImguiOverlay::init(HWND hwnd, D3DContext& d3d, float dpi_scale) {
    d3d_ = &d3d;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io    = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    io.FontGlobalScale = dpi_scale;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(d3d.device(), d3d.ctx());
    init_ = true;
}

void ImguiOverlay::render(ID3D11RenderTargetView* rtv, const OverlayModel& m, Pipeline& pipe,
                          const FrameStatsCollector& stats, const GpuProfiler& prof, double now) {
    if (!init_) return;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (visible) {
        const auto  s    = stats.snapshot(now);
        const auto& info = pipe.info();
        auto&       set  = pipe.settings;
        const ImVec4 warn(1.0f, 0.75f, 0.2f, 1.0f);

        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("rpenhance", &visible,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing);

        ImGui::Text("Source  : %s", m.source_name.c_str());
        ImGui::Text("GPU     : %s", m.adapter.c_str());
        ImGui::Text("Capture %ux%u -> sortie %ux%u @ %.0f Hz", info.capture.width, info.capture.height,
                    info.output.width, info.output.height, m.refresh_hz);
        if (info.capture.width > 0 && info.capture.height < 1000)
            ImGui::TextColored(warn, "Capture < 1080p : agrandis la fenêtre Remote Play (plein écran).");

        ImGui::SeparatorText("Cadence");
        ImGui::Text("capturé   %6.1f fps", s.captured_fps);
        ImGui::Text("unique    %6.1f fps   (doublons %4.1f %%)", s.unique_fps, s.dup_pct);
        ImGui::Text("affiché   %6.1f fps   (générées %4.1f %%)", s.present_fps, s.generated_pct);
        ImGui::Text("intervalle %5.1f ms   gigue %4.1f ms", info.interval_ms, info.jitter_ms);
        if (set.frame_gen)
            ImGui::Text("retard FG  %5.1f ms   (latence ajoutée par l'interpolation)", info.delay_ms);

        char label[64];
        std::snprintf(label, sizeof(label), "diff  %.2f / seuil %.2f", info.last_metric, set.dup_threshold);
        ImGui::PlotHistogram("##metric", stats.metric_history(), FrameStatsCollector::kHistory, stats.metric_offset(),
                             label, 0.0f, std::max(4.0f, set.dup_threshold * 3.0f), ImVec2(360, 50));
        ImGui::PlotLines("##interval", stats.interval_history(), FrameStatsCollector::kHistory,
                         stats.interval_offset(), "intervalle frames uniques (ms)", 0.0f, 80.0f, ImVec2(360, 50));

        ImGui::SeparatorText("GPU (ms)");
        ImGui::Text("analyse %.2f  upscale %.2f  flow %.2f  rendu %.2f", prof.ms(GpuSection::Analyze),
                    prof.ms(GpuSection::Upscale), prof.ms(GpuSection::Flow), prof.ms(GpuSection::Compose));

        ImGui::SeparatorText("Réglages");
        int mode = int(set.upscale);
        ImGui::Combo("Upscale", &mode, "Off (bilinéaire)\0Bicubique + netteté\0RTX Video Super Resolution\0");
        set.upscale = UpscaleMode(mode);
        ImGui::TextDisabled("actif : %s", Upscaler::path_name(info.upscale_path));
        if (set.upscale == UpscaleMode::Vsr) ImGui::TextWrapped("VSR : %s", info.vsr_status);
        if (set.upscale == UpscaleMode::Bicubic) ImGui::SliderFloat("Netteté", &set.sharpness, 0.0f, 1.0f);

        ImGui::Checkbox("Frame generation", &set.frame_gen);
        ImGui::SliderFloat("Marge du retard", &set.delay_scale, 1.0f, 2.0f, "x%.2f");
        ImGui::Checkbox("Dédoublonnage", &set.dedupe);
        ImGui::SliderFloat("Seuil doublon", &set.dup_threshold, 0.1f, 8.0f, "%.2f");
        ImGui::Checkbox("Comparer avant | après", &set.compare);
        ImGui::SameLine();
        ImGui::Checkbox("Voir le flow", &set.flow_viz);
        if (ImGui::CollapsingHeader("Synthèse (avancé)")) {
            ImGui::SliderFloat("fixe min", &set.synth.static_lo, 0.0f, 0.2f);
            ImGui::SliderFloat("fixe max", &set.synth.static_hi, 0.0f, 0.3f);
            ImGui::SliderFloat("désaccord min", &set.synth.err_lo, 0.0f, 0.3f);
            ImGui::SliderFloat("désaccord max", &set.synth.err_hi, 0.0f, 0.6f);
        }

        ImGui::Separator();
        ImGui::TextDisabled("Ctrl+Alt+ S stats  U upscale  F frame gen  C comparer  V flow  Q quitter");
        if (m.overlay_mode)
            ImGui::TextDisabled("L'overlay ne prend jamais le focus : la manette reste sur Remote Play.");
        if (ImGui::Button("Quitter")) quit_requested = true;
        ImGui::End();
    }

    ImGui::Render();
    d3d_->ctx()->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}
