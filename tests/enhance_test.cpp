// Tests de la logique de --enhance (src/pipeline/enhance_logic.cpp) : bornes des réglages, courbes, lissage,
// et références CPU des filtres GPU (débruitage bilatéral de l'étage A, netteté/tons/saturation de l'étage B)
// sur des images synthétiques. Sans GPU ni Windows.
// Compilation autonome : g++ -std=c++20 -Isrc tests/enhance_test.cpp src/pipeline/enhance_logic.cpp
#include "pipeline/enhance_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace enhance;

namespace {
int g_failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "OK   " : "ÉCHEC", what);
    if (!ok) ++g_failures;
}

bool near(float a, float b, float eps = 1e-5f) { return std::abs(a - b) <= eps; }

Image8 make(int w, int h) {
    Image8 im;
    im.w = w;
    im.h = h;
    im.px.assign(size_t(w) * size_t(h) * 4, 255);
    return im;
}
void put(Image8& im, int x, int y, float r, float g, float b) {
    uint8_t* p = im.at(x, y);
    p[2] = uint8_t(std::lround(std::clamp(r, 0.0f, 1.0f) * 255.0f));
    p[1] = uint8_t(std::lround(std::clamp(g, 0.0f, 1.0f) * 255.0f));
    p[0] = uint8_t(std::lround(std::clamp(b, 0.0f, 1.0f) * 255.0f));
}

// Mire réduite : damier 32 px modulé, texture de blocs 8x8 pseudo-aléatoire, disque rayé, barre « HUD ».
Image8 pattern(int w, int h) {
    Image8 im = make(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float chk = float(((x / 32) + (y / 32)) & 1);
            const uint32_t hb = uint32_t(x / 8) * 73856093u ^ uint32_t(y / 8) * 19349663u;
            const float m = (0.8f + 0.2f * std::sin(x * 0.09f) * std::sin(y * 0.07f)) * (0.85f + 0.3f * float(hb % 97) / 96.0f);
            float r = (0.16f + 0.62f * chk) * m, g = (0.22f + 0.50f * chk) * m, b = (0.32f + 0.26f * chk) * m;
            const float dx = float(x) - w * 0.5f, dy = float(y) - h * 0.5f;
            if (dx * dx + dy * dy < 40.0f * 40.0f) {
                const bool s = std::fmod(std::abs(dx) / 7.0f, 1.0f) < 0.5f;
                r = s ? 0.95f : 0.1f;
                g = s ? 0.55f : 0.3f;
                b = s ? 0.1f : 0.85f;
            }
            if (y > h - 24 && y < h - 8 && x > 8 && x < 120) r = g = b = ((x / 5) & 1) ? 1.0f : 0.05f;
            put(im, x, y, r, g, b);
        }
    }
    return im;
}

// Marche verticale lo -> hi adoucie sur `ramp` pixels autour de x0 (bord d'une image upscalée).
Image8 soft_step(int w, int h, int x0, float ramp, float lo, float hi) {
    Image8 im = make(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float t = std::clamp((float(x) - float(x0)) / ramp + 0.5f, 0.0f, 1.0f);
            t       = t * t * (3.0f - 2.0f * t);
            const float v = lo + (hi - lo) * t;
            put(im, x, y, v, v, v);
        }
    return im;
}

int max_gradient(const Image8& im, int y) {
    int g = 0;
    for (int x = 1; x < im.w; ++x) g = std::max(g, std::abs(int(im.at(x, y)[1]) - int(im.at(x - 1, y)[1])));
    return g;
}

void test_params() {
    Params p;
    expect(!p.enabled, "défaut : --enhance off");
    p.denoise    = -1.0f;
    p.sharpen    = 3.0f;
    p.clarity    = std::numeric_limits<float>::quiet_NaN();
    p.tone       = std::numeric_limits<float>::infinity();
    p.saturation = 9.0f;
    const Params c = clamp_params(p), d;
    expect(c.denoise == 0.0f && c.sharpen == 1.0f, "bornes 0..1");
    expect(c.clarity == d.clarity && c.tone == d.tone, "valeur non finie -> défaut");
    expect(c.saturation == kSatMax, "saturation bornée à 1.5");
    p.saturation = 0.1f;
    expect(clamp_params(p).saturation == kSatMin, "saturation bornée à 0.5");
}

void test_tone() {
    bool identity = true, fixed = true, monotone = true, bounded = true;
    for (int i = 0; i <= 1000; ++i) identity = identity && near(tone_curve(i / 1000.0f, 0.0f), i / 1000.0f);
    for (float a : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        fixed = fixed && near(tone_curve(0.0f, a), 0.0f) && near(tone_curve(0.5f, a), 0.5f) && near(tone_curve(1.0f, a), 1.0f);
        float prev = -1.0f;
        for (int i = 0; i <= 1000; ++i) {
            const float v = tone_curve(i / 1000.0f, a);
            monotone      = monotone && v > prev;
            bounded       = bounded && v >= 0.0f && v <= 1.0f;
            prev          = v;
        }
    }
    expect(identity, "courbe de tons : intensité 0 = identité");
    expect(fixed, "courbe de tons : 0, 0.5 et 1 fixes");
    expect(monotone, "courbe de tons : strictement croissante (pas d'inversion de tons)");
    expect(bounded, "courbe de tons : reste dans 0..1");
    expect(tone_curve(0.25f, 1.0f) < 0.25f && tone_curve(0.75f, 1.0f) > 0.75f, "courbe de tons : forme en S (contraste)");
    expect(std::abs(tone_curve(0.25f, 1.0f) - 0.25f) < 0.05f, "courbe de tons : douce (écart < 0.05 à intensité 1)");
}

void test_saturation() {
    float c[3] = {0.7f, 0.4f, 0.2f};
    float d[3] = {c[0], c[1], c[2]};
    saturate_rgb(d, 1.0f);
    expect(near(d[0], c[0]) && near(d[1], c[1]) && near(d[2], c[2]), "saturation 1 : identité");
    bool gray = true, luma_kept = true;
    for (float s : {0.5f, 0.8f, 1.2f, 1.5f}) {
        float g[3] = {0.4f, 0.4f, 0.4f};
        saturate_rgb(g, s);
        gray = gray && near(g[0], 0.4f) && near(g[1], 0.4f) && near(g[2], 0.4f);
        float e[3] = {c[0], c[1], c[2]};
        saturate_rgb(e, s);
        luma_kept = luma_kept && near(luma(e), luma(c), 1e-4f);
    }
    expect(gray, "saturation : un gris reste gris");
    expect(luma_kept, "saturation : luminance conservée");
    // Vibrance : gain relatif plus fort sur une couleur peu saturée.
    float lo[3] = {0.55f, 0.5f, 0.45f}, hi[3] = {0.9f, 0.2f, 0.1f};
    const float slo = lo[0] - lo[2], shi = hi[0] - hi[2];
    saturate_rgb(lo, 1.5f);
    saturate_rgb(hi, 1.5f);
    expect((lo[0] - lo[2]) / slo > (hi[0] - hi[2]) / shi, "saturation > 1 : façon vibrance (moins sur les couleurs saturées)");
    float u[3] = {0.7f, 0.4f, 0.2f};
    saturate_rgb(u, 0.5f);
    expect(near(u[0] - u[2], 0.5f * (c[0] - c[2]), 1e-4f), "saturation < 1 : désaturation uniforme");
}

void test_gains() {
    expect(sharpen_gain(0.0f, 1.0f) == 0.0f, "netteté : gain nul sur un aplat (pas d'amplification du bruit)");
    expect(sharpen_gain(0.1f, 0.0f) == 0.0f, "netteté : intensité 0 = sans effet");
    expect(sharpen_gain(0.1f, 1.0f) > sharpen_gain(0.02f, 1.0f), "netteté : gain croissant avec la texture");
    expect(sharpen_gain(0.8f, 1.0f) <= 0.25f * 1.6f + 1e-6f && sharpen_gain(0.8f, 1.0f) < sharpen_gain(0.15f, 1.0f),
           "netteté : gain réduit sur les contours très contrastés (HUD, texte)");
    bool bound = true;
    for (int i = 0; i <= 100; ++i)
        for (int j = 0; j <= 100; ++j) bound = bound && std::abs(clarity_boost(i / 100.0f, j / 100.0f, 0.0f, 1.0f)) < 0.08f;
    expect(bound, "clarté : |effet| < 0.08 à intensité 1 (bornée)");
    expect(clarity_boost(0.0f, 0.3f, 0.0f, 1.0f) == 0.0f && clarity_boost(1.0f, 0.3f, 0.0f, 1.0f) == 0.0f,
           "clarté : nulle aux extrêmes (pas d'écrêtage des noirs/blancs)");
    expect(clarity_boost(0.6f, 0.5f, 0.25f, 1.0f) == 0.0f, "clarté : coupée près d'un grand contour (anti-halo)");
    expect(clarity_boost(0.6f, 0.5f, 0.0f, 1.0f) > 0.0f && clarity_boost(0.4f, 0.5f, 0.0f, 1.0f) < 0.0f,
           "clarté : renforce l'écart à la base floue");
}

void test_smoother() {
    Smoother sm;
    Params off;
    Params e = sm.update(off, 10.0);
    expect(!e.enabled, "lissage : off dès le départ -> désactivé (chemin d'origine)");
    Params on = off;
    on.enabled = true;
    e = sm.update(on, 10.016);
    expect(e.enabled && e.sharpen > 0.0f && e.sharpen < on.sharpen, "lissage : activation en fondu");
    double t = 10.016;
    for (int i = 0; i < 200; ++i) e = sm.update(on, t += 0.016);
    expect(e.enabled && e.sharpen == on.sharpen && e.saturation == on.saturation && e.denoise == on.denoise,
           "lissage : réglages atteints exactement");
    Params on2 = on;
    on2.sharpen = 1.0f;
    e = sm.update(on2, t += 0.016);
    expect(e.sharpen > on.sharpen && e.sharpen < 1.0f, "lissage : curseur suivi en douceur");
    for (int i = 0; i < 200; ++i) e = sm.update(off, t += 0.016);
    expect(!e.enabled && e.sharpen == 0.0f && e.saturation == 1.0f, "lissage : désactivation complète après le fondu");
    Smoother s2;
    e = s2.update(on, 5.0);
    expect(e.enabled && e.sharpen == on.sharpen, "lissage : premier appel actif = réglages directs");
    e = s2.update(off, 8.0);  // pause > 1 s
    expect(!e.enabled, "lissage : pause > 1 s = sans transition");
}

void test_denoise() {
    const DenoiseCoeffs k0 = denoise_coeffs(0.0f), k1 = denoise_coeffs(0.5f), k2 = denoise_coeffs(1.0f);
    expect(k0.mix == 0.0f && k1.sigma_r < k2.sigma_r && k2.sigma_r * 255.0f <= 12.5f, "débruitage : coefficients bornés et croissants");
    const Image8 clean = pattern(256, 160);
    expect(denoise_ref(clean, 0.0f).px == clean.px, "débruitage : intensité 0 = identité");

    const Image8 deg  = degrade_blocks(clean, 12.0f, 2.0f, 7);
    const Image8 deg2 = degrade_blocks(clean, 12.0f, 2.0f, 7);
    expect(deg.px == deg2.px, "dégradation de test : déterministe");
    expect(degrade_blocks(clean, 0.0f, 0.0f, 1).px == clean.px, "dégradation de test : q = 0 sans bruit = identité");
    expect(psnr(clean, clean) == 99.0 && std::abs(ssim(clean, clean) - 1.0) < 1e-9, "PSNR/SSIM : image identique");

    const double p0 = psnr(clean, deg), s0 = ssim(clean, deg);
    std::printf("       dégradée : PSNR %.2f dB, SSIM %.4f\n", p0, s0);
    bool better = true;
    for (float s : {0.2f, 0.35f, 0.6f}) {
        const Image8 d = denoise_ref(deg, s);
        const double p = psnr(clean, d), q = ssim(clean, d);
        std::printf("       débruitage %.2f : PSNR %.2f dB, SSIM %.4f\n", s, p, q);
        better = better && p > p0 && q > s0;
    }
    expect(better, "débruitage : PSNR et SSIM vers l'image propre améliorés (0.2, 0.35, 0.6)");

    // Un vrai contour (153 niveaux) traverse le filtre sans être adouci.
    const Image8 step = soft_step(64, 8, 32, 0.01f, 0.2f, 0.8f), ds = denoise_ref(step, 1.0f);
    int diff = 0;
    for (size_t i = 0; i < step.px.size(); ++i) diff = std::max(diff, std::abs(int(step.px[i]) - int(ds.px[i])));
    expect(diff <= 1, "débruitage : contour net conservé (écart <= 1 niveau à intensité 1)");
}

void test_finish() {
    const Image8 flat = soft_step(32, 8, 100, 1.0f, 0.4f, 0.4f);
    Params sh;
    sh.sharpen = 1.0f;
    sh.tone = 0.0f;
    sh.saturation = 1.0f;
    expect(finish_ref(flat, sh).px == flat.px, "netteté : aplat inchangé");

    bool no_halo = true, steeper = true;
    for (float ramp : {2.0f, 3.0f, 5.0f}) {
        for (float amount : {0.35f, 1.0f}) {
            const Image8 st = soft_step(64, 8, 32, ramp, 0.2f, 0.8f);
            sh.sharpen      = amount;
            const Image8 o  = finish_ref(st, sh);
            int mn = 255, mx = 0;
            for (int x = 0; x < o.w; ++x) {
                mn = std::min(mn, int(o.at(x, 4)[1]));
                mx = std::max(mx, int(o.at(x, 4)[1]));
            }
            const int lo = int(std::lround(0.2f * 255.0f)), hi = int(std::lround(0.8f * 255.0f));
            std::printf("       bord (rampe %.0f px, netteté %.2f) : min %d max %d (bornes %d..%d), pente %d -> %d\n", ramp,
                        amount, mn, mx, lo, hi, max_gradient(st, 4), max_gradient(o, 4));
            no_halo = no_halo && mn >= lo && mx <= hi;
            steeper = steeper && max_gradient(o, 4) >= max_gradient(st, 4);
        }
    }
    expect(no_halo, "netteté : aucun dépassement sur un bord net (pas de halo)");
    expect(steeper, "netteté : pente du bord accentuée ou conservée");

    Params tn;
    tn.sharpen    = 0.0f;
    tn.tone       = 0.0f;
    tn.saturation = 1.0f;
    const Image8 pat = pattern(64, 64);
    expect(finish_ref(pat, tn).px == pat.px, "finition : tous les effets à 0 = identité");
}
}  // namespace

int main() {
    test_params();
    test_tone();
    test_saturation();
    test_gains();
    test_smoother();
    test_denoise();
    test_finish();
    std::printf("%s (%d échec(s))\n", g_failures ? "ÉCHEC" : "OK", g_failures);
    return g_failures ? 1 : 0;
}
