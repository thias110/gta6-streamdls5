// Tests de la détection de coupure (partie CPU) sur des images de blocs synthétiques.
// cut_stats_ref() reproduit sur CPU les sommes de cs_cut_stats (src/gpu/shaders.hpp).
// Compilation autonome : g++ -std=c++20 -Isrc tests/scene_cut_test.cpp src/pipeline/scene_cut.cpp
#include "pipeline/scene_cut.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

namespace {
constexpr int W = 60, H = 34;  // niveau le plus grossier de la pyramide pour une capture 1920x1080
using Img = std::vector<float>;

int g_failures = 0;
void expect(bool ok, const char* what, const CutFeatures& f, bool cut) {
    std::printf("%s  %-52s corr %+.3f  hist %.3f  coupure %s\n", ok ? "OK   " : "ÉCHEC", what, f.corr, f.hist,
                cut ? "oui" : "non");
    if (!ok) ++g_failures;
}

CutStatsRaw cut_stats_ref(const Img& a, const Img& b) {
    CutStatsRaw r;
    r.n = float(W * H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float va = a[size_t(y * W + x)], vb = b[size_t(y * W + x)];
            r.sa += va;
            r.sb += vb;
            r.saa += va * va;
            r.sbb += vb * vb;
            for (int k = 0; k < kCutShifts; ++k) {
                const int sx = std::clamp(x + k % 3 - 1, 0, W - 1), sy = std::clamp(y + k / 3 - 1, 0, H - 1);
                r.sab[k] += va * b[size_t(sy * W + sx)];
            }
            auto bin = [](float v) { return std::min(int(std::clamp(v, 0.0f, 1.0f) * kCutBins), kCutBins - 1); };
            ++r.ha[bin(va)];
            ++r.hb[bin(vb)];
        }
    }
    return r;
}

// Image « naturelle » lisse : dégradé + taches gaussiennes, luminance dans [lo, hi].
Img scene(unsigned seed, float lo, float hi) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    Img img(size_t(W * H));
    const float gx = u(g) - 0.5f, gy = u(g) - 0.5f;
    float cx[6], cy[6], rr[6], amp[6];
    for (int i = 0; i < 6; ++i) {
        cx[i] = u(g) * W;
        cy[i] = u(g) * H;
        rr[i] = 3.0f + u(g) * 8.0f;
        amp[i] = u(g) * 2.0f - 1.0f;
    }
    float mn = 1e9f, mx = -1e9f;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float v = gx * x / W + gy * y / H;
            for (int i = 0; i < 6; ++i) {
                const float dx = x - cx[i], dy = y - cy[i];
                v += amp[i] * std::exp(-(dx * dx + dy * dy) / (2.0f * rr[i] * rr[i]));
            }
            img[size_t(y * W + x)] = v;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
    for (float& v : img) v = lo + (v - mn) / (mx - mn) * (hi - lo);
    return img;
}

Img shifted(const Img& a, int dx, int dy) {
    Img b(a.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            b[size_t(y * W + x)] = a[size_t(std::clamp(y - dy, 0, H - 1) * W + std::clamp(x - dx, 0, W - 1))];
    return b;
}

Img noisy(Img a, float amount, unsigned seed) {
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> u(-amount, amount);
    for (float& v : a) v = std::clamp(v + u(g), 0.0f, 1.0f);
    return a;
}

// |dY| moyen (0-255), l'ancien critère.
float mean_diff(const Img& a, const Img& b) {
    double s = 0.0;
    for (size_t i = 0; i < a.size(); ++i) s += std::abs(a[i] - b[i]);
    return float(s / double(a.size()) * 255.0);
}

void check(const char* what, const Img& a, const Img& b, bool want_cut) {
    const CutParams   p;
    const CutFeatures f   = cut_features(cut_stats_ref(a, b));
    const bool        cut = is_scene_cut(mean_diff(a, b), f, p);
    expect(cut == want_cut, what, f, cut);
}

Img zoomed(const Img& a, float z) {
    Img b(a.size());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int sx = std::clamp(int(std::lround((x - W * 0.5f) / z + W * 0.5f)), 0, W - 1);
            const int sy = std::clamp(int(std::lround((y - H * 0.5f) / z + H * 0.5f)), 0, H - 1);
            b[size_t(y * W + x)] = a[size_t(sy * W + sx)];
        }
    return b;
}

// Mire de la CI au niveau grossier (un texel ~ un bloc de 32 px) : damier de 64 px (2 texels) très
// contrasté, texture multiplicative, vue à la position x (fractionnaire, interpolation linéaire) ;
// un carré de k x k texels figure un objet qui bouge autrement que la caméra.
Img mire(float x, int k) {
    std::mt19937 g(11);
    std::uniform_real_distribution<float> u(0.8f, 1.2f);
    const int WW = W + 8;
    std::vector<float> tex(size_t(WW * H));
    for (float& t : tex) t = u(g);
    auto at = [&](int wx, int y) {
        const bool chk = ((wx / 2) + (y / 2)) % 2;
        return std::clamp((chk ? 0.68f : 0.22f) * tex[size_t(y * WW + wx)], 0.0f, 1.0f);
    };
    const int   ix = int(x);
    const float t  = x - float(ix);
    Img img(size_t(W * H));
    for (int y = 0; y < H; ++y)
        for (int xx = 0; xx < W; ++xx) {
            float v = (1.0f - t) * at(xx + ix, y) + t * at(xx + ix + 1, y);
            if (k > 0 && y >= 10 && y < 10 + k && xx >= 25 && xx < 25 + k) v = 0.9f - at(xx + 5, y);
            img[size_t(y * W + xx)] = v;
        }
    return img;
}

// Taux de coupures détectées sur `n` paires de plans différents : (nouveau critère, ancien seul).
void detection_rate(const char* what, float lo, float hi, int n, double min_rate) {
    const CutParams p;
    int found = 0, legacy = 0;
    for (int s = 0; s < n; ++s) {
        const Img a = scene(1000u + unsigned(s), lo, hi), b = scene(5000u + unsigned(s), lo, hi);
        const float md = mean_diff(a, b);
        found += is_scene_cut(md, cut_features(cut_stats_ref(a, b)), p);
        legacy += md > p.mean_max;
    }
    const double rate = double(found) / n, old = double(legacy) / n;
    const bool   ok   = rate >= min_rate && rate >= old;
    std::printf("%s  %-52s détectées %.0f %% (ancien critère seul : %.0f %%)\n", ok ? "OK   " : "ÉCHEC", what,
                rate * 100.0, old * 100.0);
    if (!ok) ++g_failures;
}
}  // namespace

int main() {
    // Même plan : aucune coupure ne doit être signalée.
    const Img day = scene(1, 0.15f, 0.85f);
    for (unsigned s = 0; s < 8; ++s) {
        const Img a = scene(300 + s, 0.0f, 0.15f);
        check("même plan sombre, panoramique d'un bloc", a, noisy(shifted(a, int(s % 3) - 1, int(s / 3 % 3) - 1), 0.005f, s),
              false);
    }
    check("même plan, caméra qui bouge d'un bloc (~32 px)", day, noisy(shifted(day, 1, 0), 0.01f, 2), false);
    check("même plan, mouvement diagonal d'un bloc", day, noisy(shifted(day, 1, -1), 0.01f, 3), false);
    check("même plan, zoom x1.1", day, zoomed(day, 1.1f), false);
    check("même plan, flash (luminosité x0.8)", day, [&] { Img b = day; for (float& v : b) v *= 0.8f; return b; }(),
          false);
    {  // Panoramique rapide, même plan (CI, selftest WARP, mire x2.5 : ~35 px entre deux images) : la
       // différence moyenne dépasse mean_max mais la corrélation reste haute. Faux positif à éviter.
        const Img   a = mire(0.0f, 0), b = noisy(mire(1.12f, 7), 0.02f, 9);
        const auto  f = cut_features(cut_stats_ref(a, b));
        const float md = mean_diff(a, b);
        const bool  like_ci = md > CutParams{}.mean_max && std::abs(f.corr - 0.94f) < 0.02f && std::abs(f.hist - 0.24f) < 0.03f;
        std::printf("       (diff. moyenne %.1f > %.0f, indices proches de la CI corr 0.94 / histo 0.24 : %s)\n", md,
                    CutParams{}.mean_max, like_ci ? "oui" : "NON");
        if (!like_ci) ++g_failures;
        check("même plan, panoramique rapide ~35 px (mire CI x2.5)", a, b, false);
    }
    // Valeurs mesurées par la CI (run 44717350863), quelle que soit la différence moyenne mesurée sur GPU
    // (non journalisée) : la frame suivante x2.5 (corr 0.94, histo 0.24) est le même plan.
    for (float md : {41.0f, 60.0f, 90.0f}) {
        CutFeatures f;
        f.valid = true;
        f.corr  = 0.94f;
        f.hist  = 0.24f;
        const bool cut = is_scene_cut(md, f, CutParams{});
        char what[96];
        std::snprintf(what, sizeof(what), "CI x2.5 frame suivante, diff. moyenne %.0f", md);
        expect(!cut, what, f, cut);
    }
    // Vraie coupure x1.0 de la CI (corr 0.39, histo 0.49) : la décision ne change pas (coupure si la
    // différence moyenne dépasse mean_max, le garde-fou ne s'applique qu'aux corrélations hautes).
    {
        CutFeatures f;
        f.valid = true;
        f.corr  = 0.39f;
        f.hist  = 0.49f;
        const bool cut = is_scene_cut(60.0f, f, CutParams{});
        expect(cut, "CI x1.0 image 7 s plus loin, diff. moyenne 60", f, cut);
    }
    {  // Coupure entre plans de disposition proche mais de tonalité différente (corrélation haute,
       // histogrammes éloignés, cas vu sur les plans clairs synthétiques) : le garde-fou ne s'applique pas.
        CutFeatures f;
        f.valid = true;
        f.corr  = 0.90f;
        f.hist  = 0.44f;
        const bool cut = is_scene_cut(45.0f, f, CutParams{});
        expect(cut, "corrélation haute, histogrammes éloignés, diff. moyenne 45", f, cut);
    }
    check("deux écrans noirs (bruit léger)", noisy(Img(size_t(W * H), 0.01f), 0.002f, 5),
          noisy(Img(size_t(W * H), 0.01f), 0.002f, 6), false);

    // Plans différents.
    {
        const Img a = scene(7, 0.0f, 0.12f);
        Img       b = a;
        std::reverse(b.begin(), b.end());  // même histogramme, même luminosité, disposition retournée
        check("plans sombres, disposition retournée", a, b, true);
    }
    check("fondu depuis le noir", Img(size_t(W * H), 0.0f), day, true);
    detection_rate("coupures entre plans sombres (0-12 %)", 0.0f, 0.12f, 200, 0.5);
    detection_rate("coupures entre plans clairs (5-95 %)", 0.05f, 0.95f, 200, 0.5);

    {  // Critère désactivé : seul l'ancien (différence moyenne) reste.
        const Img a = scene(7, 0.0f, 0.12f);
        Img       b = a;
        std::reverse(b.begin(), b.end());
        CutParams p;
        p.enabled           = false;
        const CutFeatures f = cut_features(cut_stats_ref(a, b));
        const bool        cut = is_scene_cut(mean_diff(a, b), f, p);
        expect(!cut, "indices désactivés : plans sombres non détectés (attendu)", f, cut);
    }

    std::printf(g_failures ? "%d ÉCHEC(S)\n" : "TOUT OK\n", g_failures);
    return g_failures ? 1 : 0;
}
