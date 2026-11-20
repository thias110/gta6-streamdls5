#include "pipeline.hpp"

#include <cmath>

Pipeline::Pipeline(D3DContext& d3d, GpuProfiler& prof, FrameStatsCollector& stats)
    : d3d_(d3d), prof_(prof), stats_(stats) {}

void Pipeline::init() {
    comp_.init(d3d_);
    motion_.init(d3d_);
    upscaler_.init(d3d_, comp_);
}

void Pipeline::reset_history() {
    for (auto& f : ring_) {
        f.valid    = false;
        f.has_flow = false;
        f.has_raw  = false;
    }
    interval_ = jitter_ = 0.0;
    lag_ = lag_jitter_ = 0.0;
    lead_ = -1.0;
    dup_.reset();
    info_.cadence_period = 0;
}

void Pipeline::set_output_size(Size2D s) {
    if (s == output_) return;
    output_      = s;
    info_.output = s;
    reset_history();
}

Pipeline::UniqueFrame* Pipeline::newest() {
    UniqueFrame* best = nullptr;
    for (auto& f : ring_)
        if (f.valid && (!best || f.time > best->time)) best = &f;
    return best;
}

Pipeline::UniqueFrame& Pipeline::acquire_slot() {
    UniqueFrame* oldest = nullptr;
    for (auto& f : ring_) {
        if (!f.valid) return f;
        if (!oldest || f.time < oldest->time) oldest = &f;
    }
    return *oldest;
}

bool Pipeline::submit(const SourceFrame& f, double arrival) {
    if (!f.texture || output_.empty()) return false;
    auto* ctx = d3d_.ctx();
    auto* dev = d3d_.device();
    const GpuTexture& src = *f.texture;
    const Size2D cs = src.size();
    if (!(cs == capture_)) {
        reset_history();
        capture_      = cs;
        info_.capture = cs;
    }

    // 1. Doublon ? (le flux remote play est à 60 fps, le jeu souvent à 30 : une frame sur deux est répétée)
    prof_.begin(ctx, GpuSection::Analyze);
    motion_.build_pyramid(scratch_, src.srv.Get(), cs);
    UniqueFrame* last = newest();
    bool  unique = true;
    bool  cut    = false;
    float metric = -1.0f;
    if (last && (settings.dedupe || settings.frame_gen)) {
        const DiffResult d = motion_.measure_difference(scratch_, last->pyr);
        if (settings.dedupe) {
            metric = d.max_tile;
            if (f.time > last->time) {  // horodatage non monotone : rejeté plus bas, sans fausser l'apprentissage
                const auto dec = dup_.classify(metric, {settings.dup_threshold, settings.adaptive_dedupe});
                unique               = dec.unique;
                info_.dup_threshold  = dec.threshold;
                info_.cadence_period = dec.period;
            }
        }
        // Changement de plan : le flow n'a pas de sens, on n'interpolera pas à travers (HOLD).
        const CutFeatures cf = cut_features(d.cut);
        cut            = is_scene_cut(d.mean, cf, settings.cut);
        info_.cut_corr = cf.corr;
        info_.cut_hist = cf.hist;
    }
    prof_.end(ctx, GpuSection::Analyze);
    if (last && f.time <= last->time) unique = false;  // horodatage non monotone
    info_.last_metric = metric;
    stats_.on_capture(f.time, unique, metric);
    if (!unique) return false;

    // Retard d'arrivée (horodatage de capture -> soumission) : il s'ajoute au retard d'interpolation.
    const double lag = arrival - f.time;
    if (lag >= 0.0 && lag < 0.25) {
        if (lag_ == 0.0) lag_ = lag;
        lag_jitter_ = lag_jitter_ * 0.9 + std::abs(lag - lag_) * 0.1;
        lag_        = lag_ * 0.9 + lag * 0.1;
    }

    // 2. Nouvelle frame unique : on l'ajoute à l'historique.
    UniqueFrame& slot = acquire_slot();
    std::swap(slot.pyr, scratch_);
    slot.has_edges = settings.frame_gen && settings.synth.hud_protect;
    if (slot.has_edges) motion_.build_edges(slot.pyr, slot.edges);
    slot.has_raw = settings.compare;  // la copie brute ne sert qu'au mode comparaison
    if (slot.has_raw) {
        slot.raw.ensure(dev, cs.width, cs.height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, "raw");
        ctx->CopyResource(slot.raw.tex.Get(), src.tex.Get());
    }

    prof_.begin(ctx, GpuSection::Upscale);
    const GpuTexture& up = upscaler_.run(src, output_, settings.upscale, settings.sharpness);
    slot.up.ensure(dev, output_.width, output_.height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, "up");
    ctx->CopyResource(slot.up.tex.Get(), up.tex.Get());
    prof_.end(ctx, GpuSection::Upscale);
    info_.upscale_path = upscaler_.last_path();
    info_.vsr_status   = upscaler_.vsr_status();

    slot.valid    = true;
    slot.id       = next_id_++;
    slot.prev_id  = 0;
    slot.time     = f.time;
    slot.has_flow = false;
    slot.cut_before = last && cut;
    ++info_.unique_frames;

    // 3. Cadence + flow bidirectionnel avec la frame unique précédente.
    if (last) {
        const double dt = slot.time - last->time;
        if (dt > 0.004 && dt < 0.25) {
            if (interval_ == 0.0) interval_ = dt;
            jitter_   = jitter_ * 0.9 + std::abs(dt - interval_) * 0.1;
            interval_ = interval_ * 0.9 + dt * 0.1;
        }
        if (cut) ++info_.scene_cuts;
        if (!cut && (settings.frame_gen || settings.flow_viz)) {
            prof_.begin(ctx, GpuSection::Flow);
            // Prédicteurs temporels : flow de même sens de la paire précédente (mouvement ~constant).
            ID3D11ShaderResourceView* t_fwd = last->has_flow ? last->flow_fwd.srv.Get() : nullptr;
            ID3D11ShaderResourceView* t_bwd = last->has_flow ? last->flow_bwd.srv.Get() : nullptr;
            motion_.estimate_flow(last->pyr, slot.pyr, t_fwd, 1.0f, slot.flow_fwd);
            motion_.estimate_flow(slot.pyr, last->pyr, t_bwd, 1.0f, slot.flow_bwd);
            prof_.end(ctx, GpuSection::Flow);
            slot.has_flow = true;
            slot.prev_id  = last->id;
        }
    }
    info_.interval_ms = float(interval_ * 1000.0);
    info_.jitter_ms   = float(jitter_ * 1000.0);
    info_.arrival_ms  = float(lag_ * 1000.0);
    return true;
}

bool Pipeline::render(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, double now, double display_time) {
    // Avance de l'affichage sur le rendu, lissée : la cible reste calée sur les vblanks.
    const double lead = std::clamp(display_time - now, 0.0, 0.1);
    lead_ = lead_ < 0.0 ? lead : lead_ * 0.95 + lead * 0.05;

    // Retard = intervalle (avec marge) + retard d'arrivée + gigues + avance d'affichage, borné par ce que
    // l'historique couvre : au-delà, la paire encadrante serait déjà sortie de l'anneau.
    double delay = interval_ * settings.delay_scale + lag_ + 2.0 * (jitter_ + lag_jitter_) + lead_;
    if (interval_ > 0.0) delay = std::min(delay, double(kRing - 2) * interval_);
    info_.delay_ms = float(delay * 1000.0);

    std::array<const UniqueFrame*, kRing> fr{};
    size_t n = 0;
    for (const auto& u : ring_)
        if (u.valid) fr[n++] = &u;
    info_.interpolating = false;
    if (n == 0) return false;
    std::sort(fr.begin(), fr.begin() + ptrdiff_t(n), [](auto* a, auto* b) { return a->time < b->time; });
    const UniqueFrame* latest = fr[n - 1];
    const UniqueFrame* ref    = latest;

    auto* ctx = d3d_.ctx();
    prof_.begin(ctx, GpuSection::Compose);
    comp_.begin(rtv, vp);
    bool generated = false;

    if (settings.flow_viz && latest->has_flow) {
        comp_.flow_viz(latest->up.srv.Get(), latest->flow_fwd.srv.Get(), 25.0f, latest->pyr.flow_scale());
    } else if (!settings.frame_gen || n < 2 || interval_ <= 0.0) {
        comp_.blit(latest->up.srv.Get());
    } else {
        // On affiche le passé (maintenant - retard) pour toujours avoir la frame suivante B en main.
        const double target = display_time - delay;
        const UniqueFrame *a = nullptr, *b = nullptr;
        for (size_t i = 0; i + 1 < n; ++i) {
            if (fr[i]->time <= target && target < fr[i + 1]->time) {
                a = fr[i];
                b = fr[i + 1];
                break;
            }
        }
        if (!a) {
            ref = target >= latest->time ? latest : fr[0];  // B en retard (réseau) : on tient la dernière
            comp_.blit(ref->up.srv.Get());
        } else {
            const double span = b->time - a->time;
            const float  t    = float((target - a->time) / span);
            ref = t < 0.5f ? a : b;
            if (b->cut_before) {
                // HOLD : interpoler (ou basculer à mi-chemin) à travers une coupure donnerait un fondu
                // fantôme ou une coupure en avance ; A reste affichée jusqu'à l'instant de B.
                ref = a;
                comp_.blit(a->up.srv.Get());
            } else if (b->has_flow && b->prev_id == a->id && span < 0.25) {
                comp_.synth(a->up.srv.Get(), b->up.srv.Get(), b->flow_fwd.srv.Get(), b->flow_bwd.srv.Get(), t,
                            settings.synth, b->pyr.flow_scale(), a->has_edges ? a->edges.srv.Get() : nullptr,
                            b->has_edges ? b->edges.srv.Get() : nullptr);
                generated        = t > 0.05f && t < 0.95f;  // sinon quasi identique à une vraie frame
                info_.last_alpha = t;
            } else {
                comp_.blit(ref->up.srv.Get());
            }
        }
    }

    if (settings.compare && ref->has_raw) {  // avant (capture brute étirée) | après
        const LONG mid = LONG(vp.TopLeftX + vp.Width * 0.5f);
        const D3D11_RECT left{LONG(vp.TopLeftX), LONG(vp.TopLeftY), mid, LONG(vp.TopLeftY + vp.Height)};
        comp_.scissor(&left);
        comp_.blit(ref->raw.srv.Get());
        comp_.scissor(nullptr);
        const D3D11_RECT line{mid - 1, left.top, mid + 1, left.bottom};
        const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        ctx->ClearView(rtv, white, &line, 1);
    }
    comp_.end();
    prof_.end(ctx, GpuSection::Compose);

    info_.interpolating = generated;
    return generated;
}
