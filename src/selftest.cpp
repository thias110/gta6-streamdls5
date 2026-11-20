#include "selftest.hpp"

#include "capture/test_source.hpp"
#include "gpu/profiler.hpp"
#include "pipeline/compositor.hpp"
#include "pipeline/enhancer.hpp"
#include "pipeline/motion.hpp"
#include "pipeline/upscaler.hpp"

#include <cmath>

namespace {

struct Image {
    uint32_t             w = 0, h = 0;
    std::vector<uint8_t> px;  // BGRA
};

Image readback(D3DContext& d3d, const GpuTexture& t) {
    D3D11_TEXTURE2D_DESC d{};
    t.tex->GetDesc(&d);
    d.Usage          = D3D11_USAGE_STAGING;
    d.BindFlags      = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags      = 0;
    ComPtr<ID3D11Texture2D> st;
    check_hr(d3d.device()->CreateTexture2D(&d, nullptr, &st), "staging readback");
    d3d.ctx()->CopyResource(st.Get(), t.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    check_hr(d3d.ctx()->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m), "Map readback");
    Image img;
    img.w = d.Width;
    img.h = d.Height;
    img.px.resize(size_t(d.Width) * d.Height * 4);
    for (uint32_t y = 0; y < d.Height; ++y)
        std::memcpy(img.px.data() + size_t(y) * d.Width * 4, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch,
                    size_t(d.Width) * 4);
    d3d.ctx()->Unmap(st.Get(), 0);
    return img;
}

double psnr_from_mse(double mse) { return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse); }

// PSNR RGB de `test` contre `ref` ; si `b` est fourni, test = moyenne (test + b) / 2.
double psnr(const Image& ref, const Image& test, const Image* b = nullptr) {
    double se = 0.0;
    size_t n  = 0;
    for (size_t i = 0; i < ref.px.size(); i += 4) {
        for (size_t c = 0; c < 3; ++c) {
            double v = test.px[i + c];
            if (b) v = (v + b->px[i + c]) * 0.5;
            const double d = v - ref.px[i + c];
            se += d * d;
            ++n;
        }
    }
    return psnr_from_mse(se / double(n));
}

void write_bmp(const std::wstring& path, const Image& img) {
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize        = sizeof(ih);
    ih.biWidth       = LONG(img.w);
    ih.biHeight      = -LONG(img.h);  // de haut en bas
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;
    fh.bfType        = 0x4D42;
    fh.bfOffBits     = sizeof(fh) + sizeof(ih);
    fh.bfSize        = DWORD(fh.bfOffBits + img.px.size());
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::fwrite(img.px.data(), 1, img.px.size(), f);
    std::fclose(f);
}

enhance::Image8 to8(const Image& i) {
    enhance::Image8 o;
    o.w  = int(i.w);
    o.h  = int(i.h);
    o.px = i.px;
    return o;
}
Image from8(const enhance::Image8& i) {
    Image o;
    o.w  = uint32_t(i.w);
    o.h  = uint32_t(i.h);
    o.px = i.px;
    return o;
}

// Texture BGRA8 (SRV + RTV) remplie depuis le CPU.
void upload(D3DContext& d3d, GpuTexture& t, const enhance::Image8& img) {
    t.ensure(d3d.device(), uint32_t(img.w), uint32_t(img.h), DXGI_FORMAT_B8G8R8A8_UNORM,
             D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "selftest upload");
    d3d.ctx()->UpdateSubresource(t.tex.Get(), 0, nullptr, img.px.data(), UINT(img.w) * 4, 0);
}

// Plus grand dépassement (niveaux) de la composante verte hors de [lo, hi], sur toute l'image.
int overshoot(const Image& img, int lo, int hi) {
    int o = 0;
    for (size_t i = 0; i < img.px.size(); i += 4) o = std::max({o, int(img.px[i + 1]) - hi, lo - int(img.px[i + 1])});
    return o;
}

// --enhance : étage A (débruitage) sur la mire dégradée façon compression, étage B (netteté) sur un bord net.
// Résultats au journal (préfixe « Amélioration ») ; renvoie le verdict (gain du débruitage, absence de halo).
bool selftest_enhance(D3DContext& d3d, Compositor& comp, MotionEngine& me, Upscaler& up, TestPattern& tp,
                      GpuProfiler& prof, Size2D cap, Size2D out, const std::wstring& dir) {
    auto*    ctx = d3d.ctx();
    Enhancer enh;
    enh.init(d3d, comp);
    const UINT rt = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    // 1. Étage A : mire propre (sans bruit), dégradée sur CPU (blocs 8x8 quantifiés + bruit gaussien).
    GpuTexture clean, deg;
    clean.ensure(d3d.device(), cap.width, cap.height, DXGI_FORMAT_B8G8R8A8_UNORM, rt, "enh clean");
    tp.render(clean, 2.0, 0.0f, 0.0f, 1.0f);
    const Image           iclean = readback(d3d, clean);
    const enhance::Image8 c8     = to8(iclean);
    const enhance::Image8 d8     = enhance::degrade_blocks(c8, 12.0f, 2.0f, 7);
    upload(d3d, deg, d8);
    const enhance::Params defaults;
    const Image ideno = readback(d3d, enh.denoise(deg, defaults.denoise));
    const enhance::Image8 n8 = to8(ideno);
    const double p_deg = enhance::psnr(c8, d8), s_deg = enhance::ssim(c8, d8);
    const double p_den = enhance::psnr(c8, n8), s_den = enhance::ssim(c8, n8);
    const bool   den_ok = p_den > p_deg && s_den > s_deg;
    log_msg("Amélioration étage A (débruitage %.2f) : mire dégradée PSNR %.2f dB SSIM %.4f -> débruitée PSNR %.2f dB "
            "SSIM %.4f -> %s",
            defaults.denoise, p_deg, s_deg, p_den, s_den, den_ok ? "OK" : "ÉCHEC");
    {  // Écart GPU / référence CPU (indicatif : exp() et arrondis peuvent différer d'un niveau).
        const enhance::Image8 ref = enhance::denoise_ref(d8, defaults.denoise);
        int    worst = 0;
        size_t over1 = 0;
        for (size_t i = 0; i < ref.px.size(); i += 4)
            for (size_t c = 0; c < 3; ++c) {
                const int d = std::abs(int(ref.px[i + c]) - int(n8.px[i + c]));
                worst       = std::max(worst, d);
                over1 += d > 1;
            }
        log_msg("Amélioration étage A : écart GPU / référence CPU max %d niveau(x), %zu composante(s) > 1 (indicatif)",
                worst, over1);
    }
    {  // Après l'upscale (bicubique) : la dégradation est-elle moins amplifiée ?
        GpuTexture uc, ud, un;
        for (GpuTexture* t : {&uc, &ud, &un}) t->ensure(d3d.device(), out.width, out.height, DXGI_FORMAT_B8G8R8A8_UNORM, rt, "enh up");
        ctx->CopyResource(uc.tex.Get(), up.run(clean, out, UpscaleMode::Bicubic, 0.35f).tex.Get());
        ctx->CopyResource(ud.tex.Get(), up.run(deg, out, UpscaleMode::Bicubic, 0.35f).tex.Get());
        ctx->CopyResource(un.tex.Get(), up.run(enh.denoise(deg, defaults.denoise), out, UpscaleMode::Bicubic, 0.35f).tex.Get());
        const enhance::Image8 a = to8(readback(d3d, uc)), b = to8(readback(d3d, ud)), n = to8(readback(d3d, un));
        log_msg("Amélioration étage A après upscale bicubique : sans PSNR %.2f dB SSIM %.4f, avec PSNR %.2f dB SSIM %.4f "
                "(indicatif)",
                enhance::psnr(a, b), enhance::ssim(a, b), enhance::psnr(a, n), enhance::ssim(a, n));
    }
    write_bmp(dir + L"amelioration_degradee.bmp", from8(d8));
    write_bmp(dir + L"amelioration_debruitee.bmp", ideno);

    // 2. Étage B : bord net vertical + horizontal (51 -> 204), upscalé en bicubique sans netteté, puis finition.
    enhance::Image8 st;
    st.w = int(cap.width);
    st.h = int(cap.height);
    st.px.resize(size_t(st.w) * size_t(st.h) * 4);
    for (int y = 0; y < st.h; ++y)
        for (int x = 0; x < st.w; ++x) {
            uint8_t* q = st.at(x, y);
            const uint8_t v = (x >= st.w / 2) != (y >= st.h / 2) ? 204 : 51;
            q[0] = q[1] = q[2] = v;
            q[3] = 255;
        }
    GpuTexture step, stepup;
    upload(d3d, step, st);
    stepup.ensure(d3d.device(), out.width, out.height, DXGI_FORMAT_B8G8R8A8_UNORM, rt, "enh step up");
    ctx->CopyResource(stepup.tex.Get(), up.run(step, out, UpscaleMode::Bicubic, 0.0f).tex.Get());
    LumaPyramid sp;
    me.build_pyramid(sp, step.srv.Get(), cap);
    Enhancer::FinishInputs fi;
    fi.cur = &sp;
    auto finish_overshoot = [&](const enhance::Params& p) {
        return overshoot(readback(d3d, enh.finish(stepup, p, fi)), 51, 204);
    };
    enhance::Params only_sharp;
    only_sharp.clarity = 0.0f;
    only_sharp.tone = 0.0f;
    only_sharp.saturation = 1.0f;
    only_sharp.dither = false;
    const int o_in = overshoot(readback(d3d, stepup), 51, 204);
    int o_sharp = 0;
    for (float a : {defaults.sharpen, 1.0f}) {
        only_sharp.sharpen = a;
        o_sharp = std::max(o_sharp, finish_overshoot(only_sharp));
    }
    const bool halo_ok = o_sharp <= std::max(1, o_in);
    enhance::Params only_clar = only_sharp;
    only_clar.sharpen = 0.0f;
    only_clar.clarity = 1.0f;
    enhance::Params dflt = defaults;
    dflt.tone = 0.0f;
    dflt.saturation = 1.0f;  // tons et saturation déplacent les niveaux 51/204 sans être des halos
    log_msg("Amélioration étage B, halo sur un bord net 51 -> 204 : entrée upscalée %d niveau(x), netteté %.2f et 1 : "
            "%d -> %s ; clarté 1 : %d, netteté+clarté par défaut + dithering : %d (indicatifs)",
            o_in, defaults.sharpen, o_sharp, halo_ok ? "OK" : "ÉCHEC", finish_overshoot(only_clar),
            finish_overshoot(dflt));

    // Image de la mire débruitée, upscalée et finie avec les réglages d'origine (à regarder).
    {
        LumaPyramid dp;
        me.build_pyramid(dp, deg.srv.Get(), cap);
        Enhancer::FinishInputs mfi;
        mfi.cur = &dp;
        const GpuTexture& u = up.run(enh.denoise(deg, defaults.denoise), out, UpscaleMode::Bicubic, 0.35f);
        write_bmp(dir + L"amelioration_finie.bmp", readback(d3d, enh.finish(u, defaults, mfi)));
    }

    // 3. Coût GPU (réglages d'origine) : étage A en résolution de capture, étage B en sortie.
    for (int i = 0; i < 30; ++i) {
        prof.begin_frame(ctx);
        prof.begin(ctx, GpuSection::Denoise);
        enh.denoise(deg, defaults.denoise);
        prof.end(ctx, GpuSection::Denoise);
        prof.begin(ctx, GpuSection::Finish);
        enh.finish(stepup, defaults, fi);
        prof.end(ctx, GpuSection::Finish);
        prof.end_frame(ctx);
        ctx->Flush();
        Sleep(2);
    }
    log_msg("GPU : amélioration étage A %ux%u %.3f ms, étage B %ux%u %.3f ms", cap.width, cap.height,
            prof.ms(GpuSection::Denoise), out.width, out.height, prof.ms(GpuSection::Finish));
    return den_ok && halo_ok;
}

}  // namespace

int run_selftest(const Config& cfg) {
    log_msg("== rpenhance selftest ==");
    D3DContext d3d;
    d3d.initialize(cfg.debug_layer, cfg.warp);
    auto* dev = d3d.device();
    auto* ctx = d3d.ctx();

    Compositor comp;
    comp.init(d3d);
    MotionEngine me;
    me.init(d3d);
    // Backend NVIDIA VFX (si compilé) : mesuré à part, n'entre pas dans le verdict OK/ÉCHEC. Résultats à
    // usage local : vérifier la licence NVIDIA avant de publier des chiffres de VFG.
    NvvfxBackend nvvfx;
    {
        NvvfxOptions o;
        o.sdk_dir   = cfg.nvvfx_dir;
        o.model_dir = cfg.nvvfx_models;
        o.vfg_mode  = uint32_t(cfg.vfg_mode);
        o.vsr_mode  = uint32_t(cfg.nvvfx_vsr_mode);
        nvvfx.set_options(o);
    }
    Upscaler up;
    up.init(d3d, comp, &nvvfx);
    TestPattern tp;
    tp.init(d3d, comp);
    GpuProfiler prof;
    prof.init(dev);

    const Size2D cap{1920, 1080}, out{2560, 1440};
    const UINT   rt = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    GpuTexture A, B, GT, An, Far, Aup, Bup, GTup, Syn, Viz;
    for (GpuTexture* t : {&A, &B, &GT, &An, &Far}) t->ensure(dev, cap.width, cap.height, DXGI_FORMAT_B8G8R8A8_UNORM, rt, "st");
    for (GpuTexture* t : {&Aup, &Bup, &GTup, &Syn, &Viz})
        t->ensure(dev, out.width, out.height, DXGI_FORMAT_B8G8R8A8_UNORM, rt, "st up");
    LumaPyramid pa, pb, pan, pfar;
    GpuTexture  fab, fba, ea, eb;
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(out.width), float(out.height), 0.0f, 1.0f};

    const std::wstring dir = exe_directory() + L"selftest\\";
    CreateDirectoryW(dir.c_str(), nullptr);

    bool ok = true;
    const float noise = 0.012f;
    const double dt   = 1.0 / 30.0;
    for (float speed : {1.0f, 2.5f}) {
        const double t0 = 2.0;
        tp.render(A, t0, noise, 1, speed);
        tp.render(B, t0 + dt, noise, 2, speed);
        tp.render(GT, t0 + dt * 0.5, noise, 3, speed);
        tp.render(An, t0, noise, 4, speed);  // même image, autre bruit : un "doublon" réencodé

        me.build_pyramid(pa, A.srv.Get(), cap);
        me.build_pyramid(pb, B.srv.Get(), cap);
        me.build_pyramid(pan, An.srv.Get(), cap);
        const DiffResult dup = me.measure_difference(pa, pan);
        const DiffResult uni = me.measure_difference(pa, pb);
        const bool dedupe_ok = dup.max_tile <= cfg.dup_threshold && uni.max_tile > cfg.dup_threshold;
        log_msg("[vitesse x%.1f] diff doublon %.2f, diff nouvelle frame %.2f (seuil %.2f) -> %s", speed, dup.max_tile,
                uni.max_tile, cfg.dup_threshold, dedupe_ok ? "OK" : "ÉCHEC");

        // Coupures : la frame suivante du même plan ne doit pas en être une (vérifié). L'image 7 s plus
        // loin n'est qu'indicative : la mire est quasi périodique (damier), sa disposition peut se ressembler.
        tp.render(Far, t0 + 7.0, noise, 5, speed);
        me.build_pyramid(pfar, Far.srv.Get(), cap);
        const CutParams   cutp;
        const CutFeatures same_f = cut_features(uni.cut);
        const DiffResult  far_d  = me.measure_difference(pa, pfar);
        const CutFeatures far_f  = cut_features(far_d.cut);
        const bool same_cut = is_scene_cut(uni.mean, same_f, cutp), far_cut = is_scene_cut(far_d.mean, far_f, cutp);
        const bool cut_ok   = !same_cut;
        log_msg("[vitesse x%.1f] coupure : frame suivante diff. moy. %.1f corr %.2f histo %.2f -> %s (%s) ; image 7 s "
                "plus loin diff. moy. %.1f corr %.2f histo %.2f -> %s (indicatif)",
                speed, uni.mean, same_f.corr, same_f.hist, same_cut ? "coupure" : "même plan", cut_ok ? "OK" : "ÉCHEC",
                far_d.mean, far_f.corr, far_f.hist, far_cut ? "coupure" : "même plan");

        me.estimate_flow_pair(pa, pb, nullptr, 1.0f, fab, nullptr, 1.0f, fba);
        me.build_edges(pa, ea);
        me.build_edges(pb, eb);

        auto upscale_to = [&](const GpuTexture& src, GpuTexture& dst) {
            const GpuTexture& u = up.run(src, out, UpscaleMode::Bicubic, 0.0f);
            ctx->CopyResource(dst.tex.Get(), u.tex.Get());
        };
        upscale_to(A, Aup);
        upscale_to(B, Bup);
        upscale_to(GT, GTup);

        // Sans puis avec la protection du HUD (masques de bords), pour mesurer son effet.
        comp.begin(Syn.rtv.Get(), vp);
        comp.synth(Aup.srv.Get(), Bup.srv.Get(), fab.srv.Get(), fba.srv.Get(), 0.5f, SynthParams{}, pa.flow_scale());
        comp.end();
        const Image iS0 = readback(d3d, Syn);
        comp.begin(Syn.rtv.Get(), vp);
        comp.synth(Aup.srv.Get(), Bup.srv.Get(), fab.srv.Get(), fba.srv.Get(), 0.5f, SynthParams{}, pa.flow_scale(),
                   ea.srv.Get(), eb.srv.Get());
        comp.end();

        const Image iA = readback(d3d, Aup), iB = readback(d3d, Bup), iGT = readback(d3d, GTup),
                    iS = readback(d3d, Syn);
        const double p_near  = psnr(iGT, iA);
        const double p_blend = psnr(iGT, iA, &iB);
        const double p_synth = psnr(iGT, iS);
        const bool   interp_ok = p_synth > p_blend + 1.0 && p_synth > p_near + 1.0;
        log_msg("[vitesse x%.1f] PSNR frame du milieu : répétition %.2f dB, fondu %.2f dB, interpolation %.2f dB -> %s",
                speed, p_near, p_blend, p_synth, interp_ok ? "OK" : "ÉCHEC");
        log_msg("[vitesse x%.1f] protection du HUD : %.2f dB sans, %.2f dB avec", speed, psnr(iGT, iS0), p_synth);

        {  // --flow-scale 0.5 (indicatif) : flow à résolution réduite, synthèse en pleine résolution.
            LumaPyramid ha, hb;
            GpuTexture  hfab, hfba, hea, heb;
            me.build_pyramid(ha, A.srv.Get(), cap, 0.5f);
            me.build_pyramid(hb, B.srv.Get(), cap, 0.5f);
            me.estimate_flow_pair(ha, hb, nullptr, 1.0f, hfab, nullptr, 1.0f, hfba);
            me.build_edges(ha, hea);
            me.build_edges(hb, heb);
            comp.begin(Syn.rtv.Get(), vp);
            comp.synth(Aup.srv.Get(), Bup.srv.Get(), hfab.srv.Get(), hfba.srv.Get(), 0.5f, SynthParams{},
                       ha.flow_scale(), hea.srv.Get(), heb.srv.Get());
            comp.end();
            log_msg("[vitesse x%.1f] flow-scale 0.5 : interpolation %.2f dB (1.0 : %.2f dB)", speed,
                    psnr(iGT, readback(d3d, Syn)), p_synth);
        }
        if (NvvfxBackend::compiled_in()) {  // VFG du SDK NVIDIA sur la même paire (indicatif)
            nvvfx.release_textures();
            if (nvvfx.interpolate(d3d, Aup, 1, Bup, 2, 0.5f, Syn))
                log_msg("[vitesse x%.1f] VFG NVIDIA (indicatif) : %.2f dB (flow maison %.2f dB)", speed,
                        psnr(iGT, readback(d3d, Syn)), p_synth);
            else
                log_msg("[vitesse x%.1f] VFG NVIDIA indisponible : %s", speed, nvvfx.vfg_status());
        }
        ok = ok && dedupe_ok && interp_ok && cut_ok;

        if (speed == 1.0f) {
            comp.begin(Viz.rtv.Get(), vp);
            comp.flow_viz(Aup.srv.Get(), fab.srv.Get(), 25.0f, pa.flow_scale());
            comp.end();
            write_bmp(dir + L"a.bmp", iA);
            write_bmp(dir + L"b.bmp", iB);
            write_bmp(dir + L"verite_milieu.bmp", iGT);
            write_bmp(dir + L"interpolee.bmp", iS);
            write_bmp(dir + L"flow.bmp", readback(d3d, Viz));
        }
    }

    // Coût GPU d'une frame unique (pyramide + 2 flows) et d'une synthèse 1440p.
    for (int i = 0; i < 60; ++i) {
        prof.begin_frame(ctx);
        prof.begin(ctx, GpuSection::Analyze);
        me.build_pyramid(pb, B.srv.Get(), cap);
        prof.end(ctx, GpuSection::Analyze);
        prof.begin(ctx, GpuSection::Flow);
        // Les deux sens dans les mêmes Dispatch (comme le pipeline). Le prédicteur du retour est le fab de
        // l'itération précédente (avant : celui qu'on venait d'écrire) ; entrées fixes, donc équivalent.
        me.estimate_flow_pair(pa, pb, fab.srv.Get(), 1.0f, fab, fab.srv.Get(), -1.0f, fba);
        prof.end(ctx, GpuSection::Flow);
        prof.begin(ctx, GpuSection::Compose);
        comp.begin(Syn.rtv.Get(), vp);
        comp.synth(Aup.srv.Get(), Bup.srv.Get(), fab.srv.Get(), fba.srv.Get(), 0.5f, SynthParams{}, pa.flow_scale(),
                   ea.srv.Get(), eb.srv.Get());
        comp.end();
        prof.end(ctx, GpuSection::Compose);
        prof.end_frame(ctx);
        ctx->Flush();
        Sleep(2);
    }
    log_msg("GPU : pyramide %.3f ms, flow aller+retour %.3f ms, synthèse 1440p %.3f ms", prof.ms(GpuSection::Analyze),
            prof.ms(GpuSection::Flow), prof.ms(GpuSection::Compose));

    // --enhance (étages A et B) : qualité mesurée sur la mire, coût GPU.
    const bool enh_ok = selftest_enhance(d3d, comp, me, up, tp, prof, cap, out, dir);
    ok = ok && enh_ok;

    // RTX VSR : on vérifie que la voie VideoProcessor fonctionne et que le driver accepte l'extension.
    up.run(A, out, UpscaleMode::Vsr, 0.0f);
    log_msg("Upscale VSR : voie %s ; %s", Upscaler::path_name(up.last_path()), up.vsr_status());
    if (NvvfxBackend::compiled_in()) {
        up.run(A, out, UpscaleMode::Nvvfx, 0.0f);
        log_msg("Upscale nvvfx : voie %s ; %s", Upscaler::path_name(up.last_path()), up.vsr_status());
    }

    log_msg("Images écrites dans %s", narrow(dir).c_str());
    log_msg(ok ? "SELFTEST OK" : "SELFTEST ÉCHEC");
    return ok ? 0 : 1;
}
