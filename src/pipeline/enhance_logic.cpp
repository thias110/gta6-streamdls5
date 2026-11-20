#include "enhance_logic.hpp"

#include <algorithm>
#include <cmath>

namespace enhance {

namespace {
float bounded(float v, float lo, float hi, float fallback) {
    if (!std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

float approach(float cur, float target, float a) {
    const float v = cur + (target - cur) * a;
    return std::abs(target - v) < 1e-3f ? target : v;
}
}  // namespace

Params clamp_params(const Params& p) {
    const Params d;
    Params r     = p;
    r.denoise    = bounded(p.denoise, 0.0f, 1.0f, d.denoise);
    r.sharpen    = bounded(p.sharpen, 0.0f, 1.0f, d.sharpen);
    r.clarity    = bounded(p.clarity, 0.0f, 1.0f, d.clarity);
    r.tone       = bounded(p.tone, 0.0f, 1.0f, d.tone);
    r.saturation = bounded(p.saturation, kSatMin, kSatMax, d.saturation);
    return r;
}

DenoiseCoeffs denoise_coeffs(float strength) {
    const float s = std::isfinite(strength) ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
    DenoiseCoeffs c;
    c.sigma_r = (2.0f + 10.0f * s) / 255.0f;  // 2 à 12 niveaux
    c.sigma_s = 1.0f + 0.6f * s;
    c.mix     = std::min(1.0f, 1.5f * s);
    return c;
}

Image8 denoise_ref(const Image8& src, float strength) {
    Image8 out = src;
    const DenoiseCoeffs k = denoise_coeffs(strength);
    if (k.mix <= 0.0f || src.w <= 0 || src.h <= 0) return out;
    const float inv_2ss2 = 1.0f / (2.0f * k.sigma_s * k.sigma_s);
    const float inv_2sr2 = 1.0f / (2.0f * k.sigma_r * k.sigma_r);
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            const uint8_t* cp = src.at(x, y);
            float c[3] = {cp[0] / 255.0f, cp[1] / 255.0f, cp[2] / 255.0f};
            float acc[3] = {0.0f, 0.0f, 0.0f}, wsum = 0.0f;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const uint8_t* sp = src.at(std::clamp(x + dx, 0, src.w - 1), std::clamp(y + dy, 0, src.h - 1));
                    float s[3] = {sp[0] / 255.0f, sp[1] / 255.0f, sp[2] / 255.0f};
                    float d2 = 0.0f;
                    for (int i = 0; i < 3; ++i) d2 += (s[i] - c[i]) * (s[i] - c[i]);
                    const float w = std::exp(-float(dx * dx + dy * dy) * inv_2ss2 - d2 * (1.0f / 3.0f) * inv_2sr2);
                    for (int i = 0; i < 3; ++i) acc[i] += s[i] * w;
                    wsum += w;
                }
            }
            uint8_t* op = out.at(x, y);
            for (int i = 0; i < 3; ++i) {
                const float v = c[i] + (acc[i] / wsum - c[i]) * k.mix;
                op[i] = uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
            }
            op[3] = 255;
        }
    }
    return out;
}

namespace {
float smoothstep(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float sat01(float v) { return std::clamp(v, 0.0f, 1.0f); }
}  // namespace

float sharpen_gain(float range, float amount) {
    return amount * 1.6f * smoothstep(0.01f, 0.04f, range) * (1.0f - 0.75f * smoothstep(0.2f, 0.5f, range));
}

float clarity_boost(float y, float base, float spread, float amount) {
    const float d = y - base;
    const float b = amount * 0.8f * d / (1.0f + std::abs(d) * 10.0f);
    return b * sat01(4.0f * y * (1.0f - y)) * (1.0f - smoothstep(0.06f, 0.2f, spread));
}

float tone_curve(float x, float amount) {
    const float a = 0.5f * amount;
    return x - a * x * (1.0f - x) * (1.0f - 2.0f * x);
}

float luma(const float rgb[3]) { return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f; }

void saturate_rgb(float rgb[3], float s) {
    const float y  = luma(rgb);
    const float mx = std::max({rgb[0], rgb[1], rgb[2]}), mn = std::min({rgb[0], rgb[1], rgb[2]});
    const float k  = s >= 1.0f ? 1.0f + (s - 1.0f) * (1.0f - sat01(mx - mn)) : s;
    for (int i = 0; i < 3; ++i) rgb[i] = y + (rgb[i] - y) * k;
}

Image8 finish_ref(const Image8& src, const Params& p) {
    Image8 out = src;
    if (src.w <= 0 || src.h <= 0) return out;
    // RGB 0..1 (Image8 est en B, G, R, A)
    auto load = [&](int x, int y, float c[3]) {
        const uint8_t* q = src.at(std::clamp(x, 0, src.w - 1), std::clamp(y, 0, src.h - 1));
        c[0] = q[2] / 255.0f;
        c[1] = q[1] / 255.0f;
        c[2] = q[0] / 255.0f;
    };
    static const int kOff[8][2] = {{0, -1}, {0, 1}, {1, 0}, {-1, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int y = 0; y < src.h; ++y) {
        for (int x = 0; x < src.w; ++x) {
            float c[3];
            load(x, y, c);
            if (p.sharpen > 0.0f) {
                float mn[3] = {c[0], c[1], c[2]}, mx[3] = {c[0], c[1], c[2]}, avg[3] = {0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 8; ++k) {
                    float n[3];
                    load(x + kOff[k][0], y + kOff[k][1], n);
                    const float wgt = k < 4 ? 0.2f : 0.05f;
                    for (int i = 0; i < 3; ++i) {
                        mn[i] = std::min(mn[i], n[i]);
                        mx[i] = std::max(mx[i], n[i]);
                        avg[i] += n[i] * wgt;
                    }
                }
                const float range = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]});
                const float k     = sharpen_gain(range, p.sharpen);
                for (int i = 0; i < 3; ++i) c[i] = std::clamp(c[i] + k * (c[i] - avg[i]), mn[i], mx[i]);
            }
            if (p.tone > 0.0f) {
                const float yl = sat01(luma(c));
                const float dy = tone_curve(yl, p.tone) - yl;
                for (int i = 0; i < 3; ++i) c[i] += dy;
            }
            if (p.saturation != 1.0f) saturate_rgb(c, p.saturation);
            uint8_t* o = out.at(x, y);
            o[2] = uint8_t(std::lround(sat01(c[0]) * 255.0f));
            o[1] = uint8_t(std::lround(sat01(c[1]) * 255.0f));
            o[0] = uint8_t(std::lround(sat01(c[2]) * 255.0f));
            o[3] = 255;
        }
    }
    return out;
}

namespace {
// xorshift64* : suite identique sur toutes les plateformes (std::normal_distribution ne l'est pas).
struct Rng {
    uint64_t s;
    explicit Rng(uint32_t seed) : s(0x9E3779B97F4A7C15ull ^ (uint64_t(seed) * 0xD1B54A32D192ED03ull)) {
        if (!s) s = 1;
    }
    double uniform() {  // ]0, 1[
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (double((s * 0x2545F4914F6CDD1Dull) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }
    double gauss() {
        const double u = uniform(), v = uniform();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(6.283185307179586 * v);
    }
};

double luma8(const uint8_t* p) { return 0.2126 * p[2] + 0.7152 * p[1] + 0.0722 * p[0]; }
}  // namespace

Image8 degrade_blocks(const Image8& src, float q, float noise, uint32_t seed) {
    Image8 out = src;
    double basis[8][8];  // basis[k][x] = c(k) cos((2x + 1) k pi / 16) / 2 (DCT-II orthonormale)
    for (int k = 0; k < 8; ++k)
        for (int x = 0; x < 8; ++x)
            basis[k][x] = (k ? 0.5 : std::sqrt(0.125)) * std::cos((2 * x + 1) * k * 3.141592653589793 / 16.0);
    for (int by = 0; by + 8 <= src.h; by += 8) {
        for (int bx = 0; bx + 8 <= src.w; bx += 8) {
            for (int ch = 0; ch < 3; ++ch) {
                double b[8][8], t[8][8], c[8][8];
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) b[y][x] = double(src.at(bx + x, by + y)[ch]) - 128.0;
                for (int y = 0; y < 8; ++y)  // lignes
                    for (int u = 0; u < 8; ++u) {
                        double a = 0.0;
                        for (int x = 0; x < 8; ++x) a += basis[u][x] * b[y][x];
                        t[y][u] = a;
                    }
                for (int v = 0; v < 8; ++v)  // colonnes + quantification
                    for (int u = 0; u < 8; ++u) {
                        double a = 0.0;
                        for (int y = 0; y < 8; ++y) a += basis[v][y] * t[y][u];
                        const double st = (u + v == 0) ? 8.0 : double(q) * (1.0 + 0.5 * (u + v));
                        c[v][u] = q > 0.0f ? std::round(a / st) * st : a;  // q <= 0 : pas de quantification
                    }
                for (int y = 0; y < 8; ++y)  // inverse : colonnes
                    for (int u = 0; u < 8; ++u) {
                        double a = 0.0;
                        for (int v = 0; v < 8; ++v) a += basis[v][y] * c[v][u];
                        t[y][u] = a;
                    }
                for (int y = 0; y < 8; ++y)  // inverse : lignes
                    for (int x = 0; x < 8; ++x) {
                        double a = 0.0;
                        for (int u = 0; u < 8; ++u) a += basis[u][x] * t[y][u];
                        out.at(bx + x, by + y)[ch] = uint8_t(std::clamp(std::lround(a + 128.0), 0L, 255L));
                    }
            }
        }
    }
    if (noise > 0.0f) {
        Rng rng(seed);
        for (size_t i = 0; i < out.px.size(); i += 4)
            for (int ch = 0; ch < 3; ++ch)
                out.px[i + size_t(ch)] =
                    uint8_t(std::clamp(std::lround(double(out.px[i + size_t(ch)]) + rng.gauss() * noise), 0L, 255L));
    }
    return out;
}

double psnr(const Image8& ref, const Image8& test) {
    double se = 0.0;
    size_t n  = 0;
    const size_t sz = std::min(ref.px.size(), test.px.size());
    for (size_t i = 0; i + 3 < sz; i += 4)
        for (size_t c = 0; c < 3; ++c) {
            const double d = double(ref.px[i + c]) - double(test.px[i + c]);
            se += d * d;
            ++n;
        }
    if (!n) return 0.0;
    const double mse = se / double(n);
    return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

double ssim(const Image8& ref, const Image8& test) {
    const double c1 = (0.01 * 255.0) * (0.01 * 255.0), c2 = (0.03 * 255.0) * (0.03 * 255.0);
    const int    w = std::min(ref.w, test.w), h = std::min(ref.h, test.h);
    double total = 0.0;
    int    n     = 0;
    for (int by = 0; by + 8 <= h; by += 8) {
        for (int bx = 0; bx + 8 <= w; bx += 8) {
            double sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x) {
                    const double a = luma8(ref.at(bx + x, by + y)), b = luma8(test.at(bx + x, by + y));
                    sa += a;
                    sb += b;
                    saa += a * a;
                    sbb += b * b;
                    sab += a * b;
                }
            const double ma = sa / 64.0, mb = sb / 64.0;
            const double va = saa / 64.0 - ma * ma, vb = sbb / 64.0 - mb * mb, cov = sab / 64.0 - ma * mb;
            total += ((2.0 * ma * mb + c1) * (2.0 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2));
            ++n;
        }
    }
    return n ? total / n : 0.0;
}

Params Smoother::update(const Params& target_in, double now, float tau) {
    const Params target = clamp_params(target_in);
    const double dt     = now - last_;
    last_               = now;
    float a             = 1.0f;
    if (init_ && dt >= 0.0 && dt <= 1.0 && tau > 0.0f) a = 1.0f - std::exp(-float(dt) / tau);
    init_ = true;

    // Activation / désactivation : on fait varier mix ; les réglages suivent leur cible pendant que l'effet est actif
    // (à l'arrêt, on garde les derniers pour une sortie en fondu sans changer de rendu en chemin).
    const float prev_mix = mix_;
    mix_ = approach(mix_, target.enabled ? 1.0f : 0.0f, a);
    if (target.enabled) {
        if (a >= 1.0f || prev_mix <= 0.0f) {
            cur_ = target;  // entrée en fondu depuis 0 : directement les bons réglages
        } else {
            cur_.denoise    = approach(cur_.denoise, target.denoise, a);
            cur_.sharpen    = approach(cur_.sharpen, target.sharpen, a);
            cur_.clarity    = approach(cur_.clarity, target.clarity, a);
            cur_.tone       = approach(cur_.tone, target.tone, a);
            cur_.saturation = approach(cur_.saturation, target.saturation, a);
        }
        cur_.dither = target.dither;
    }

    Params e;
    e.enabled    = mix_ > 0.0f;
    e.denoise    = cur_.denoise * mix_;
    e.sharpen    = cur_.sharpen * mix_;
    e.clarity    = cur_.clarity * mix_;
    e.tone       = cur_.tone * mix_;
    e.saturation = 1.0f + (cur_.saturation - 1.0f) * mix_;
    e.dither     = cur_.dither;
    return e;
}

}  // namespace enhance
