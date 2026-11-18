#include "imgui_overlay.hpp"
#include "../capture/audio_passthrough.hpp"

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

void ImguiOverlay::render(ID3D11RenderTargetView* rtv, OverlayModel& m, Pipeline& pipe,
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
            ImGui::TextColored(warn, m.hdmi ? "Capture < 1080p : choisis un meilleur mode (--mode, --list-devices)."
                                            : "Capture < 1080p : agrandis la fenêtre Remote Play (plein écran).");
        if (m.hdmi && m.audio) {
            ImGui::Text("Son : %s (%.0f ms en tampon)", m.audio->name().c_str(), m.audio->buffered_ms());
            if (ImGui::SliderFloat("Volume", &m.audio_volume, 0.0f, 1.0f, "%.2f")) m.audio->set_volume(m.audio_volume);
        } else if (m.hdmi) {
            ImGui::TextColored(warn, "Son non relayé : --audio-device \"nom\" (voir --list-devices).");
        }

        ImGui::SeparatorText("Cadence");
        ImGui::Text("capturé   %6.1f fps", s.captured_fps);
        ImGui::Text("unique    %6.1f fps   (doublons %4.1f %%)", s.unique_fps, s.dup_pct);
        ImGui::Text("affiché   %6.1f fps   (générées %4.1f %%)", s.present_fps, s.generated_pct);
        ImGui::Text("intervalle %5.1f ms   gigue %4.1f ms", info.interval_ms, info.jitter_ms);
        if (set.frame_gen)
            ImGui::Text("retard FG  %5.1f ms   (latence ajoutée par l'interpolation)", info.delay_ms);
        ImGui::Text("arrivée    %5.1f ms   changements de plan %llu", info.arrival_ms,
                    static_cast<unsigned long long>(info.scene_cuts));
        ImGui::TextDisabled("coupure    corr. blocs %+.2f   dist. histo %.2f", info.cut_corr, info.cut_hist);

        const float thr = info.dup_threshold > 0.0f ? info.dup_threshold : set.dup_threshold;
        if (set.dedupe && set.adaptive_dedupe) {
            if (info.cadence_period > 0)
                ImGui::Text("rythme     verrouillé : 1 image sur %d captures", info.cadence_period);
            else
                ImGui::TextDisabled("rythme     libre");
        }
        char label[64];
        std::snprintf(label, sizeof(label), "diff  %.2f / seuil %.2f%s", info.last_metric, thr,
                      set.adaptive_dedupe ? " (adaptatif)" : "");
        ImGui::PlotHistogram("##metric", stats.metric_history(), FrameStatsCollector::kHistory, stats.metric_offset(),
                             label, 0.0f, std::max(4.0f, thr * 3.0f), ImVec2(360, 50));
        ImGui::PlotLines("##interval", stats.interval_history(), FrameStatsCollector::kHistory,
                         stats.interval_offset(), "intervalle frames uniques (ms)", 0.0f, 80.0f, ImVec2(360, 50));

        ImGui::SeparatorText("GPU (ms)");
        ImGui::TextDisabled("lectures de mesure bloquantes : %llu", static_cast<unsigned long long>(info.blocking_reads));
        ImGui::Text("analyse %.2f  upscale %.2f  flow %.2f  rendu %.2f", prof.ms(GpuSection::Analyze),
                    prof.ms(GpuSection::Upscale), prof.ms(GpuSection::Flow), prof.ms(GpuSection::Compose));

        ImGui::SeparatorText("Réglages");
        const bool nv = NvvfxBackend::compiled_in();
        int mode = int(set.upscale);
        ImGui::Combo("Upscale", &mode,
                     nv ? "Off (bilinéaire)\0Bicubique + netteté\0RTX Video Super Resolution\0NVIDIA VFX SDK (VSR)\0"
                        : "Off (bilinéaire)\0Bicubique + netteté\0RTX Video Super Resolution\0");
        set.upscale = UpscaleMode(mode);
        ImGui::TextDisabled("actif : %s", Upscaler::path_name(info.upscale_path));
        if (set.upscale == UpscaleMode::Vsr || set.upscale == UpscaleMode::Nvvfx)
            ImGui::TextWrapped("VSR : %s", info.vsr_status);
        if (set.upscale == UpscaleMode::Bicubic) ImGui::SliderFloat("Netteté", &set.sharpness, 0.0f, 1.0f);

        ImGui::Checkbox("Frame generation", &set.frame_gen);
        if (nv) {
            int im = int(set.interp);
            ImGui::Combo("Interpolation", &im, "Flow maison\0NVIDIA VFX SDK (VFG)\0");
            set.interp = InterpMode(im);
            if (set.interp == InterpMode::Nvvfx) {
                ImGui::TextDisabled("actif : %s", info.interp_nvvfx ? "VFG (SDK NVIDIA)" : "flow maison (repli)");
                ImGui::TextWrapped("VFG : %s", info.vfg_status);
                ImGui::TextDisabled("temps GPU de VFG non compté ci-dessus (flux CUDA)");
            }
        }
        ImGui::SliderFloat("Marge du retard", &set.delay_scale, 1.0f, 2.0f, "x%.2f");
        // Appliquée au relâchement : chaque changement d'échelle vide l'historique des frames.
        if (!flow_scale_editing_) flow_scale_edit_ = set.flow_scale;
        ImGui::SliderFloat("Échelle du flow", &flow_scale_edit_, 0.5f, 1.0f, "x%.2f");
        flow_scale_editing_ = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) set.flow_scale = flow_scale_edit_;
        ImGui::TextDisabled("flow estimé en %ux%u, synthèse en %ux%u", info.flow_res.width, info.flow_res.height,
                            info.output.width, info.output.height);
        ImGui::Checkbox("Dédoublonnage", &set.dedupe);
        ImGui::Checkbox("Seuil adaptatif + rythme", &set.adaptive_dedupe);
        ImGui::SliderFloat(set.adaptive_dedupe ? "Seuil doublon (base)" : "Seuil doublon", &set.dup_threshold, 0.1f,
                           8.0f, "%.2f");
        ImGui::Checkbox("Comparer avant | après", &set.compare);
        ImGui::SameLine();
        ImGui::Checkbox("Voir le flow", &set.flow_viz);
        if (ImGui::CollapsingHeader("Synthèse (avancé)")) {
            ImGui::SliderFloat("fixe min", &set.synth.static_lo, 0.0f, 0.2f);
            ImGui::SliderFloat("fixe max", &set.synth.static_hi, 0.0f, 0.3f);
            ImGui::SliderFloat("désaccord min", &set.synth.err_lo, 0.0f, 0.3f);
            ImGui::SliderFloat("désaccord max", &set.synth.err_hi, 0.0f, 0.6f);
            ImGui::Checkbox("Protéger le HUD (bords fixes)", &set.synth.hud_protect);
            ImGui::SliderFloat("HUD : tolérance min", &set.synth.hud_lo, 0.0f, 0.2f);
            ImGui::SliderFloat("HUD : tolérance max", &set.synth.hud_hi, 0.0f, 0.3f);
        }
        if (ImGui::CollapsingHeader("Changements de plan (avancé)")) {
            ImGui::Checkbox("Corrélation + histogramme (plans sombres)", &set.cut.enabled);
            ImGui::SliderFloat("diff. moyenne max", &set.cut.mean_max, 5.0f, 100.0f, "%.0f");
            ImGui::SliderFloat("corrélation max", &set.cut.corr_max, -0.5f, 0.9f, "%.2f");
            ImGui::SliderFloat("distance histo min", &set.cut.hist_min, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("corrélation seule max", &set.cut.corr_strong, -0.5f, 0.5f, "%.2f");
            ImGui::SliderFloat("même plan : corrélation min", &set.cut.corr_same, 0.5f, 1.0f, "%.2f");
            ImGui::SliderFloat("même plan : distance histo max", &set.cut.hist_same, 0.0f, 1.0f, "%.2f");
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
