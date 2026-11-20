#pragma once
// Sources HLSL compilées au lancement avec D3DCompile (d3dcompiler_47.dll est fourni par Windows).
// Chaque source est précédée de kCommon.

namespace shaders {

inline constexpr char kCommon[] = R"HLSL(
SamplerState g_linear : register(s0);
SamplerState g_point  : register(s1);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
float max3(float3 v) { return max(v.x, max(v.y, v.z)); }
)HLSL";

inline constexpr char kFullscreenVS[] = R"HLSL(
VSOut vs_fullscreen(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv  = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Copie / étirement bilinéaire
inline constexpr char kBlit[] = R"HLSL(
Texture2D<float4> g_src : register(t0);
float4 ps_blit(VSOut i) : SV_Target {
    return float4(g_src.SampleLevel(g_linear, i.uv, 0).rgb, 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Upscale Catmull-Rom (9 taps bilinéaires) + netteté bornée par le voisinage 2x2 (pas de halo).
inline constexpr char kUpscale[] = R"HLSL(
Texture2D<float4> g_src : register(t0);
cbuffer UpscaleCB : register(b0) {
    float2 g_src_size;
    float2 g_inv_src;
    float  g_sharp;
    float3 g_upad;
};

float3 catmull_rom(float2 uv) {
    float2 pos = uv * g_src_size;
    float2 tc1 = floor(pos - 0.5) + 0.5;
    float2 f   = pos - tc1;
    float2 w0  = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1  = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2  = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3  = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 tc0  = (tc1 - 1.0) * g_inv_src;
    float2 tc3  = (tc1 + 2.0) * g_inv_src;
    float2 tc12 = (tc1 + w2 / w12) * g_inv_src;

    float3 r = 0.0;
    r += g_src.SampleLevel(g_linear, float2(tc0.x,  tc0.y),  0).rgb * w0.x  * w0.y;
    r += g_src.SampleLevel(g_linear, float2(tc12.x, tc0.y),  0).rgb * w12.x * w0.y;
    r += g_src.SampleLevel(g_linear, float2(tc3.x,  tc0.y),  0).rgb * w3.x  * w0.y;
    r += g_src.SampleLevel(g_linear, float2(tc0.x,  tc12.y), 0).rgb * w0.x  * w12.y;
    r += g_src.SampleLevel(g_linear, float2(tc12.x, tc12.y), 0).rgb * w12.x * w12.y;
    r += g_src.SampleLevel(g_linear, float2(tc3.x,  tc12.y), 0).rgb * w3.x  * w12.y;
    r += g_src.SampleLevel(g_linear, float2(tc0.x,  tc3.y),  0).rgb * w0.x  * w3.y;
    r += g_src.SampleLevel(g_linear, float2(tc12.x, tc3.y),  0).rgb * w12.x * w3.y;
    r += g_src.SampleLevel(g_linear, float2(tc3.x,  tc3.y),  0).rgb * w3.x  * w3.y;
    return r;
}

float4 ps_upscale(VSOut i) : SV_Target {
    float3 c  = catmull_rom(i.uv);
    float2 tc = (floor(i.uv * g_src_size - 0.5) + 0.5) * g_inv_src;
    float3 a = g_src.SampleLevel(g_point, tc, 0).rgb;
    float3 b = g_src.SampleLevel(g_point, tc + float2(g_inv_src.x, 0.0), 0).rgb;
    float3 d = g_src.SampleLevel(g_point, tc + float2(0.0, g_inv_src.y), 0).rgb;
    float3 e = g_src.SampleLevel(g_point, tc + g_inv_src, 0).rgb;
    float3 mn = min(min(a, b), min(d, e));
    float3 mx = max(max(a, b), max(d, e));
    c += g_sharp * (c - (a + b + d + e) * 0.25);
    return float4(clamp(c, mn, mx), 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// --enhance, étage A (avant l'upscale) : filtre bilatéral 5x5 sur la capture (taille identique à la cible).
// Lisse le bruit et les marches de blocs de compression (écarts de quelques niveaux) sans toucher aux vrais
// contours (écarts de dizaines de niveaux) ; l'upscale ne les amplifie donc pas. Référence CPU et choix des
// coefficients : enhance::denoise_ref / denoise_coeffs (src/pipeline/enhance_logic.cpp).
inline constexpr char kDenoise[] = R"HLSL(
Texture2D<float4> g_dn_src : register(t0);
cbuffer DenoiseCB : register(b0) {
    float g_dn_inv_2ss2;  // 1 / (2 sigma_s²), sigma_s en pixels
    float g_dn_inv_2sr2;  // 1 / (2 sigma_r²), sigma_r en écart de couleur 0..1
    float g_dn_mix;       // part du filtré dans la sortie
    float g_dn_pad;
};

float4 ps_denoise(VSOut i) : SV_Target {
    uint w, h;
    g_dn_src.GetDimensions(w, h);
    int2   m = int2(w, h) - 1;
    int2   p = int2(i.pos.xy);
    float3 c = g_dn_src.Load(int3(p, 0)).rgb;
    float3 acc  = 0.0;
    float  wsum = 0.0;
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            float3 s  = g_dn_src.Load(int3(clamp(p + int2(dx, dy), 0, m), 0)).rgb;
            float3 d  = s - c;
            float  wt = exp(-float(dx * dx + dy * dy) * g_dn_inv_2ss2 - dot(d, d) * (1.0 / 3.0) * g_dn_inv_2sr2);
            acc  += s * wt;
            wsum += wt;
        }
    }
    return float4(lerp(c, acc / wsum, g_dn_mix), 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// --enhance, étage B (après l'upscale, à la taille de sortie, BGRA8 -> BGRA8), dans cet ordre :
//  1. netteté adaptative : c + k (c - moyenne pondérée 3x3), k réduit sur les zones plates (bruit) et sur les
//     contours déjà très contrastés (texte, HUD), puis borné au min/max du voisinage 3x3 : aucun dépassement,
//     donc aucun halo, par construction (principe « contrast adaptive » d'AMD FidelityFX CAS, MIT ; code et
//     formules propres à rpenhance, voir THIRD-PARTY-NOTICES.md) ;
//  2. clarté : luminance - base floue à grand rayon (niveau 1 de la pyramide de luminance, ~8 px de capture),
//     écrêtée en douceur, atténuée dans les ombres/hautes lumières et là où la base varie fort (grand contour à
//     proximité : c'est là que naîtraient les halos d'un unsharp à grand rayon) ;
//  3. courbe de tons douce en S sur la luminance (points fixes 0, 0.5 et 1, pente >= 1 - tone/2 : monotone) ;
//  4. saturation : < 1 uniforme ; > 1 façon « vibrance » (moins de gain sur les couleurs déjà saturées) ;
//  5. dithering triangulaire de +-1 niveau sur les seuls pixels modifiés (évite le banding des dégradés).
// HUD / texte fixe : près d'un bord (masque de bords existant, sinon Sobel sur le niveau 0) et stable depuis la
// frame unique précédente, netteté et clarté sont réduites à 25 %.
// Références CPU et constantes : enhance::sharpen_gain, clarity_boost, tone_curve, saturate_rgb, finish_ref.
inline constexpr char kFinish[] = R"HLSL(
Texture2D<float4> g_fin_src  : register(t0);
Texture2D<float>  g_fin_l0   : register(t1);  // luminance, niveau 0 de la pyramide de cette frame
Texture2D<float>  g_fin_lp   : register(t2);  // luminance, niveau 0 de la frame unique précédente (stabilité)
Texture2D<float>  g_fin_base : register(t3);  // luminance, niveau 1 : base floue de la clarté
Texture2D<float>  g_fin_edge : register(t4);  // masque de bords dilaté (cs_edge_*), si déjà calculé
cbuffer FinishCB : register(b0) {
    float g_fin_sharpen;
    float g_fin_clarity;
    float g_fin_tone;
    float g_fin_sat;
    float g_fin_dither;
    float g_fin_hud;       // 1 : protection du HUD (bords stables)
    float g_fin_has_prev;  // g_fin_lp valide
    float g_fin_has_edge;  // g_fin_edge valide (sinon Sobel sur g_fin_l0)
    float g_fin_has_pyr;   // g_fin_l0 / g_fin_base valides
    float3 g_fin_pad;
};

float min3c(float3 v) { return min(v.x, min(v.y, v.z)); }

// enhance::sharpen_gain
float sharpen_gain(float range, float amount) {
    return amount * 1.6 * smoothstep(0.01, 0.04, range) * (1.0 - 0.75 * smoothstep(0.2, 0.5, range));
}
// enhance::clarity_boost
float clarity_boost(float y, float base, float spread, float amount) {
    float d = y - base;
    float b = amount * 0.8 * d / (1.0 + abs(d) * 10.0);
    return b * saturate(4.0 * y * (1.0 - y)) * (1.0 - smoothstep(0.06, 0.2, spread));
}
// enhance::tone_curve
float tone_curve(float x, float amount) {
    float a = 0.5 * amount;
    return x - a * x * (1.0 - x) * (1.0 - 2.0 * x);
}
// enhance::saturate_rgb
float3 saturate_rgb(float3 c, float s) {
    float y = luma(c);
    float k = s >= 1.0 ? 1.0 + (s - 1.0) * (1.0 - saturate(max3(c) - min3c(c))) : s;
    return y + (c - y) * k;
}
float ehash(uint2 p) {
    uint h = p.x * 1973u + p.y * 9277u + 26699u;
    h = (h ^ (h >> 15)) * 0x2c1b3c6du;
    h = (h ^ (h >> 12)) * 0x297a2d39u;
    h ^= h >> 15;
    return float(h & 0xffffu) * (1.0 / 65535.0);
}

float3 fin_load(int2 p, int2 m) { return g_fin_src.Load(int3(clamp(p, 0, m), 0)).rgb; }

float4 ps_finish(VSOut i) : SV_Target {
    uint w, h;
    g_fin_src.GetDimensions(w, h);
    int2   m  = int2(w, h) - 1;
    int2   p  = int2(i.pos.xy);
    float2 uv = i.uv;
    float3 c0 = fin_load(p, m);
    float3 c  = c0;

    // Masque HUD : bord net (dans la capture) et stable depuis la frame unique précédente.
    float protect = 1.0;
    if (g_fin_hud > 0.5 && g_fin_has_pyr > 0.5 && g_fin_has_prev > 0.5) {
        float edge;
        if (g_fin_has_edge > 0.5) {
            edge = g_fin_edge.SampleLevel(g_linear, uv, 0);
        } else {
            uint lw, lh;
            g_fin_l0.GetDimensions(lw, lh);
            float2 o  = 1.0 / float2(lw, lh);
            float tl = g_fin_l0.SampleLevel(g_linear, uv + float2(-o.x, -o.y), 0);
            float tc = g_fin_l0.SampleLevel(g_linear, uv + float2(0.0, -o.y), 0);
            float tr = g_fin_l0.SampleLevel(g_linear, uv + float2(o.x, -o.y), 0);
            float ml = g_fin_l0.SampleLevel(g_linear, uv + float2(-o.x, 0.0), 0);
            float mr = g_fin_l0.SampleLevel(g_linear, uv + float2(o.x, 0.0), 0);
            float bl = g_fin_l0.SampleLevel(g_linear, uv + float2(-o.x, o.y), 0);
            float bc = g_fin_l0.SampleLevel(g_linear, uv + float2(0.0, o.y), 0);
            float br = g_fin_l0.SampleLevel(g_linear, uv + float2(o.x, o.y), 0);
            float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
            float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
            edge = smoothstep(4.0 * 16.0 / 255.0, 4.0 * 32.0 / 255.0, abs(gx) + abs(gy));  // seuils de cs_edge_sobel
        }
        float dl    = abs(g_fin_l0.SampleLevel(g_linear, uv, 0) - g_fin_lp.SampleLevel(g_linear, uv, 0));
        float still = 1.0 - smoothstep(0.02, 0.06, dl);
        protect = 1.0 - 0.75 * edge * still;
    }

    // 1. Netteté adaptative bornée au voisinage 3x3.
    if (g_fin_sharpen > 0.0) {
        float3 n  = fin_load(p + int2(0, -1), m), s  = fin_load(p + int2(0, 1), m);
        float3 e  = fin_load(p + int2(1, 0), m),  wv = fin_load(p + int2(-1, 0), m);
        float3 nw = fin_load(p + int2(-1, -1), m), ne = fin_load(p + int2(1, -1), m);
        float3 sw = fin_load(p + int2(-1, 1), m),  se = fin_load(p + int2(1, 1), m);
        float3 mn  = min(c, min(min(min(n, s), min(e, wv)), min(min(nw, ne), min(sw, se))));
        float3 mx  = max(c, max(max(max(n, s), max(e, wv)), max(max(nw, ne), max(sw, se))));
        float3 avg = (n + s + e + wv) * 0.2 + (nw + ne + sw + se) * 0.05;
        float  k   = sharpen_gain(max3(mx - mn), g_fin_sharpen) * protect;
        c = clamp(c + k * (c - avg), mn, mx);
    }

    // 2. Clarté (contraste local, luminance seule).
    if (g_fin_clarity > 0.0 && g_fin_has_pyr > 0.5) {
        uint bw, bh;
        g_fin_base.GetDimensions(bw, bh);
        float2 o  = 1.0 / float2(bw, bh);
        float b0 = g_fin_base.SampleLevel(g_linear, uv + float2(-o.x, -o.y), 0);
        float b1 = g_fin_base.SampleLevel(g_linear, uv + float2( o.x, -o.y), 0);
        float b2 = g_fin_base.SampleLevel(g_linear, uv + float2(-o.x,  o.y), 0);
        float b3 = g_fin_base.SampleLevel(g_linear, uv + float2( o.x,  o.y), 0);
        float base   = (b0 + b1 + b2 + b3) * 0.25;
        float spread = max(max(b0, b1), max(b2, b3)) - min(min(b0, b1), min(b2, b3));
        c += clarity_boost(saturate(luma(c)), base, spread, g_fin_clarity) * protect;
    }

    // 3. Courbe de tons (luminance, décalage additif : la chrominance ne bouge pas).
    if (g_fin_tone > 0.0) {
        float y = saturate(luma(c));
        c += tone_curve(y, g_fin_tone) - y;
    }

    // 4. Saturation / vibrance.
    if (g_fin_sat != 1.0) c = saturate_rgb(c, g_fin_sat);

    c = saturate(c);
    // 5. Dithering triangulaire (+-1 niveau) sur les pixels modifiés seulement.
    if (g_fin_dither > 0.0 && max3(abs(c - c0)) > 0.25 / 255.0)
        c = saturate(c + (ehash(uint2(p)) + ehash(uint2(p) + uint2(37u, 17u)) - 1.0) * (1.0 / 255.0));
    return float4(c, 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Pyramide de luminance (niveau 0 = demi-résolution de la capture)
inline constexpr char kLuma[] = R"HLSL(
Texture2D<float4>  g_rgb  : register(t0);
Texture2D<float>   g_lsrc : register(t1);
RWTexture2D<float> g_ldst : register(u0);
cbuffer LumaCB : register(b0) {
    uint2 g_dst_size;
    uint2 g_src_size;
};

[numthreads(8, 8, 1)]
void cs_luma_from_rgb(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_dst_size.x || id.y >= g_dst_size.y) return;
    int2 m = int2(g_src_size) - 1;
    int2 p = int2(id.xy) * 2;
    float l = luma(g_rgb.Load(int3(min(p, m), 0)).rgb)
            + luma(g_rgb.Load(int3(min(p + int2(1, 0), m), 0)).rgb)
            + luma(g_rgb.Load(int3(min(p + int2(0, 1), m), 0)).rgb)
            + luma(g_rgb.Load(int3(min(p + int2(1, 1), m), 0)).rgb);
    g_ldst[id.xy] = l * 0.25;
}

// Niveau 0 à échelle réduite (--flow-scale < 1) : facteur de réduction entre 2 et 4, donc 4 lectures
// bilinéaires aux quarts de l'empreinte du texel de destination (moyenne d'environ 4x4 texels source).
[numthreads(8, 8, 1)]
void cs_luma_from_rgb_scaled(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_dst_size.x || id.y >= g_dst_size.y) return;
    float2 inv = 1.0 / float2(g_dst_size);
    float2 uv  = (float2(id.xy) + 0.5) * inv;
    float2 o   = 0.25 * inv;
    float l = luma(g_rgb.SampleLevel(g_linear, uv + float2(-o.x, -o.y), 0).rgb)
            + luma(g_rgb.SampleLevel(g_linear, uv + float2( o.x, -o.y), 0).rgb)
            + luma(g_rgb.SampleLevel(g_linear, uv + float2(-o.x,  o.y), 0).rgb)
            + luma(g_rgb.SampleLevel(g_linear, uv + float2( o.x,  o.y), 0).rgb);
    g_ldst[id.xy] = l * 0.25;
}

[numthreads(8, 8, 1)]
void cs_luma_down(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_dst_size.x || id.y >= g_dst_size.y) return;
    int2 m = int2(g_src_size) - 1;
    int2 p = int2(id.xy) * 2;
    float l = g_lsrc.Load(int3(min(p, m), 0))
            + g_lsrc.Load(int3(min(p + int2(1, 0), m), 0))
            + g_lsrc.Load(int3(min(p + int2(0, 1), m), 0))
            + g_lsrc.Load(int3(min(p + int2(1, 1), m), 0));
    g_ldst[id.xy] = l * 0.25;
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Détection de doublons : moyenne |dY| par tuile 16x16, max et somme sur l'image (x1000, 0-255).
inline constexpr char kTileDiff[] = R"HLSL(
Texture2D<float>    g_da   : register(t0);
Texture2D<float>    g_db   : register(t1);
RWByteAddressBuffer g_dout : register(u0);
cbuffer DiffCB : register(b0) {
    uint2 g_dsize;
    uint2 g_dpad;
};
groupshared float gs_sum[256];

[numthreads(16, 16, 1)]
void cs_tile_diff(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
    float d = 0.0;
    if (id.x < g_dsize.x && id.y < g_dsize.y)
        d = abs(g_da.Load(int3(id.xy, 0)) - g_db.Load(int3(id.xy, 0)));
    gs_sum[gi] = d;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1) {
        if (gi < s) gs_sum[gi] += gs_sum[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi == 0) {
        uint2 valid = min(uint2(16, 16), g_dsize - gid.xy * 16);
        float mean  = gs_sum[0] / float(valid.x * valid.y) * 255.0;
        uint  v     = (uint)(mean * 1000.0);
        uint  prev;
        g_dout.InterlockedMax(0, v, prev);
        g_dout.InterlockedAdd(4, v, prev);
        g_dout.InterlockedAdd(8, 1, prev);
    }
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Statistiques de changement de plan sur le niveau le plus grossier de la pyramide (chaque texel =
// moyenne d'un bloc de ~32x32 pixels) : sommes pour la corrélation de luminance par blocs, testée
// sous 9 décalages d'un bloc (le mouvement ordinaire ne la fait pas chuter), et histogrammes 32 cases.
// La corrélation ignore la luminosité globale : elle reste parlante entre deux plans sombres.
// Un seul groupe de 256 threads. Sortie (uint, à partir de l'octet 16 du tampon de mesure) :
//   [4] n  [5] somme a  [6] somme b  [7] somme a2  [8] somme b2  [9..17] somme a*b(décalage)  (floats)
//   [18..49] histogramme a  [50..81] histogramme b
inline constexpr char kCutStats[] = R"HLSL(
Texture2D<float>    g_ca   : register(t0);
Texture2D<float>    g_cb   : register(t1);
RWByteAddressBuffer g_cout : register(u0);
cbuffer CutCB : register(b0) {
    uint2 g_csize;
    uint2 g_cpad;
};
#define CUT_BINS 32
groupshared float gs_red[256];
groupshared uint  gs_ha[CUT_BINS];
groupshared uint  gs_hb[CUT_BINS];

float reduce256(float v, uint gi) {
    gs_red[gi] = v;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1) {
        if (gi < s) gs_red[gi] += gs_red[gi + s];
        GroupMemoryBarrierWithGroupSync();
    }
    float r = gs_red[0];
    GroupMemoryBarrierWithGroupSync();
    return r;
}

[numthreads(256, 1, 1)]
void cs_cut_stats(uint gi : SV_GroupIndex) {
    if (gi < CUT_BINS) {
        gs_ha[gi] = 0;
        gs_hb[gi] = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    int2  hi = int2(g_csize) - 1;
    uint  total = g_csize.x * g_csize.y;
    float sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0;
    float sab[9] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    for (uint i = gi; i < total; i += 256) {
        int2  p = int2(i % g_csize.x, i / g_csize.x);
        float a = g_ca.Load(int3(p, 0));
        float b = g_cb.Load(int3(p, 0));
        sa += a;
        sb += b;
        saa += a * a;
        sbb += b * b;
        [unroll] for (int k = 0; k < 9; ++k) {
            int2 o = int2(k % 3 - 1, k / 3 - 1);
            sab[k] += a * g_cb.Load(int3(clamp(p + o, 0, hi), 0));
        }
        uint dummy;
        InterlockedAdd(gs_ha[min(uint(saturate(a) * CUT_BINS), CUT_BINS - 1)], 1, dummy);
        InterlockedAdd(gs_hb[min(uint(saturate(b) * CUT_BINS), CUT_BINS - 1)], 1, dummy);
    }

    float r_sa  = reduce256(sa, gi);
    float r_sb  = reduce256(sb, gi);
    float r_saa = reduce256(saa, gi);
    float r_sbb = reduce256(sbb, gi);
    float r_sab[9];
    [unroll] for (int k = 0; k < 9; ++k) r_sab[k] = reduce256(sab[k], gi);

    if (gi == 0) {
        g_cout.Store(16, asuint(float(total)));
        g_cout.Store(20, asuint(r_sa));
        g_cout.Store(24, asuint(r_sb));
        g_cout.Store(28, asuint(r_saa));
        g_cout.Store(32, asuint(r_sbb));
        [unroll] for (int k = 0; k < 9; ++k) g_cout.Store(36 + 4 * k, asuint(r_sab[k]));
    }
    if (gi < CUT_BINS) {
        g_cout.Store(72 + 4 * gi, gs_ha[gi]);
        g_cout.Store(72 + 4 * CUT_BINS + 4 * gi, gs_hb[gi]);
    }
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Masque de bords pour la protection du HUD (niveau 0 de la pyramide, demi-résolution) :
// Sobel 3x3 sur la luminance (norme L1, une marche de hauteur h donne 4h), adouci entre
// g_edge_lo et g_edge_hi, puis dilaté (max 3x3) pour couvrir les deux côtés d'un trait.
inline constexpr char kEdges[] = R"HLSL(
Texture2D<float>   g_esrc : register(t0);
RWTexture2D<float> g_edst : register(u0);
cbuffer EdgeCB : register(b0) {
    uint2 g_esize;
    float g_edge_lo;
    float g_edge_hi;
};

float eload(int2 p) { return g_esrc.Load(int3(clamp(p, 0, int2(g_esize) - 1), 0)); }

[numthreads(8, 8, 1)]
void cs_edge_sobel(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_esize.x || id.y >= g_esize.y) return;
    int2 p = int2(id.xy);
    float tl = eload(p + int2(-1, -1)), tc = eload(p + int2(0, -1)), tr = eload(p + int2(1, -1));
    float ml = eload(p + int2(-1,  0)),                                mr = eload(p + int2(1,  0));
    float bl = eload(p + int2(-1,  1)), bc = eload(p + int2(0,  1)), br = eload(p + int2(1,  1));
    float gx = (tr + 2.0 * mr + br) - (tl + 2.0 * ml + bl);
    float gy = (bl + 2.0 * bc + br) - (tl + 2.0 * tc + tr);
    g_edst[id.xy] = smoothstep(g_edge_lo, g_edge_hi, abs(gx) + abs(gy));
}

[numthreads(8, 8, 1)]
void cs_edge_dilate(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_esize.x || id.y >= g_esize.y) return;
    float m = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx) m = max(m, eload(int2(id.xy) + int2(dx, dy)));
    g_edst[id.xy] = m;
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Estimation de mouvement par blocs 8x8, grossier -> fin.
// Flow en UV normalisés : X(p) ~ Y(p + flow).
// Les deux sens d'une paire partagent chaque Dispatch (SV_DispatchThreadID.z) :
//   z = 0 : X = g_ma, Y = g_mb, prédicteurs g_coarse / g_temporal, sortie g_flow_out  (A -> B)
//   z = 1 : X = g_mb, Y = g_ma, prédicteurs g_coarse1 / g_temporal1, sortie g_flow_out1 (B -> A)
// z est uniforme dans un groupe (numthreads z = 1) : les branches sur z ne divergent pas.
// Un Dispatch de profondeur 1 ne calcule que le sens 0.
inline constexpr char kMotion[] = R"HLSL(
Texture2D<float>    g_ma        : register(t0);
Texture2D<float>    g_mb        : register(t1);
Texture2D<float2>   g_coarse    : register(t2);
Texture2D<float2>   g_temporal  : register(t3);
Texture2D<float2>   g_coarse1   : register(t4);
Texture2D<float2>   g_temporal1 : register(t5);
RWTexture2D<float2> g_flow_out  : register(u0);
RWTexture2D<float2> g_flow_out1 : register(u1);

cbuffer MotionCB : register(b0) {
    int2   g_msize;
    int2   g_grid;
    float2 g_inv_msize;
    int    g_radius;
    uint   g_flags;
    float  g_temporal_sign;   // sens 0
    float  g_lambda;
    float  g_zero_bias;
    float  g_temporal_sign1;  // sens 1
    // UV image -> UV de grille : la grille compte div_up(taille, 8) blocs, donc couvre un peu plus
    // que l'image quand la taille n'est pas multiple de 8 (scale = taille / (grille * 8)).
    float2 g_coarse_scale;    // pour g_coarse (niveau L+1)
    float2 g_temporal_scale;  // pour g_temporal (niveau 0 de la paire précédente)
};

#define BS 8
#define FLAG_COARSE    1u
#define FLAG_TEMPORAL  2u   // prédicteur temporel du sens 0
#define FLAG_SUBPIXEL  4u
#define FLAG_TEMPORAL1 8u   // prédicteur temporel du sens 1

// SAD d'un bloc 8x8 : 16 GatherRed par image (2x2 texels chacun) au lieu de 64 Load.
// Gather au coin commun des texels p et p + (1, 1) : uv = (p + 1) / taille, centre exact de
// l'empreinte 2x2 (marge d'un demi-texel). Composantes : w = (p.x, p.y), z = (p.x+1, p.y),
// x = (p.x, p.y+1), y = (p.x+1, p.y+1). g_point est en adressage CLAMP : chaque texel hors image est
// borné comme clamp(p, 0, taille - 1) avec Load. Valeurs exactes (pas de filtrage) et même ordre de
// sommation (ligne par ligne, de gauche à droite) : résultat identique à la version Load.
float block_sad(Texture2D<float> mx, Texture2D<float> my, int2 origin, int2 v) {
    float s = 0.0;
    [loop] for (int y = 0; y < BS; y += 2) {
        float4 gx[BS / 2];
        float4 gy[BS / 2];
        [unroll] for (int k = 0; k < BS / 2; ++k) {
            int2 c = origin + int2(2 * k + 1, y + 1);
            gx[k] = mx.GatherRed(g_point, float2(c) * g_inv_msize);
            gy[k] = my.GatherRed(g_point, float2(c + v) * g_inv_msize);
        }
        [unroll] for (int k = 0; k < BS / 2; ++k) {
            s += abs(gx[k].w - gy[k].w);
            s += abs(gx[k].z - gy[k].z);
        }
        [unroll] for (int k = 0; k < BS / 2; ++k) {
            s += abs(gx[k].x - gy[k].x);
            s += abs(gx[k].y - gy[k].y);
        }
    }
    return s * (1.0 / (BS * BS));
}

float motion_cost(Texture2D<float> mx, Texture2D<float> my, int2 origin, int2 v, int2 pred) {
    float c = block_sad(mx, my, origin, v);
    int2 d = abs(v - pred);
    c += g_lambda * float(d.x + d.y);
    if (v.x == 0 && v.y == 0) c -= g_zero_bias;
    return c;
}

int2 to_px(float2 f) { return int2(round(f * float2(g_msize))); }

float2 search_block(Texture2D<float> mx, Texture2D<float> my, Texture2D<float2> coarse, Texture2D<float2> temporal,
                    bool use_temporal, float temporal_sign, uint2 cell) {
    int2   origin = int2(cell) * BS;
    float2 cuv    = (float2(origin) + BS * 0.5) * g_inv_msize;
    float2 nstep  = 2.0 * BS * g_inv_msize;  // un bloc du niveau grossier

    int2 pred = int2(0, 0);
    int2 cands[7];
    uint n = 0;
    cands[n++] = int2(0, 0);
    if (g_flags & FLAG_COARSE) {
        pred = to_px(coarse.SampleLevel(g_linear, cuv * g_coarse_scale, 0));
        cands[n++] = pred;
        cands[n++] = to_px(coarse.SampleLevel(g_point, (cuv + float2(nstep.x, 0.0)) * g_coarse_scale, 0));
        cands[n++] = to_px(coarse.SampleLevel(g_point, (cuv - float2(nstep.x, 0.0)) * g_coarse_scale, 0));
        cands[n++] = to_px(coarse.SampleLevel(g_point, (cuv + float2(0.0, nstep.y)) * g_coarse_scale, 0));
        cands[n++] = to_px(coarse.SampleLevel(g_point, (cuv - float2(0.0, nstep.y)) * g_coarse_scale, 0));
    }
    if (use_temporal)
        cands[n++] = to_px(temporal_sign * temporal.SampleLevel(g_linear, cuv * g_temporal_scale, 0));

    float best = 1e9;
    int2  bv   = int2(0, 0);
    [loop] for (uint i = 0; i < n; ++i) {
        float c = motion_cost(mx, my, origin, cands[i], pred);
        if (c < best) { best = c; bv = cands[i]; }
    }

    int2 base = bv;
    [loop] for (int dy = -g_radius; dy <= g_radius; ++dy) {
        [loop] for (int dx = -g_radius; dx <= g_radius; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int2  v = base + int2(dx, dy);
            float c = motion_cost(mx, my, origin, v, pred);
            if (c < best) { best = c; bv = v; }
        }
    }

    return float2(bv) * g_inv_msize;
}

[numthreads(8, 8, 1)]
void cs_motion_search(uint3 id : SV_DispatchThreadID) {
    if (int(id.x) >= g_grid.x || int(id.y) >= g_grid.y) return;
    if (id.z == 0)
        g_flow_out[id.xy] = search_block(g_ma, g_mb, g_coarse, g_temporal, (g_flags & FLAG_TEMPORAL) != 0,
                                         g_temporal_sign, id.xy);
    else
        g_flow_out1[id.xy] = search_block(g_mb, g_ma, g_coarse1, g_temporal1, (g_flags & FLAG_TEMPORAL1) != 0,
                                          g_temporal_sign1, id.xy);
}

float2 subpixel(Texture2D<float> mx, Texture2D<float> my, int2 origin, int2 bv) {
    float2 fv = float2(bv);
    float c0 = block_sad(mx, my, origin, bv);
    float cl = block_sad(mx, my, origin, bv - int2(1, 0));
    float cr = block_sad(mx, my, origin, bv + int2(1, 0));
    float cu = block_sad(mx, my, origin, bv - int2(0, 1));
    float cd = block_sad(mx, my, origin, bv + int2(0, 1));
    float ddx = cl + cr - 2.0 * c0;
    float ddy = cu + cd - 2.0 * c0;
    if (ddx > 1e-5) fv.x += clamp(0.5 * (cl - cr) / ddx, -0.5, 0.5);
    if (ddy > 1e-5) fv.y += clamp(0.5 * (cu - cd) / ddy, -0.5, 0.5);
    return fv;
}

// Propagation (façon PatchMatch) : chaque bloc essaie les vecteurs de ses voisins du même niveau.
// Les bons vecteurs des zones sans ambiguïté envahissent les zones répétitives ou peu texturées.
// Entrée : g_coarse (sens 0) / g_coarse1 (sens 1) = flow du même niveau.
static const int2 kNeighbours[12] = {
    int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0), int2(1, 0), int2(-1, 1),
    int2(0, 1), int2(1, 1), int2(0, -3), int2(0, 3), int2(-3, 0), int2(3, 0)
};

float2 propagate_block(Texture2D<float> mx, Texture2D<float> my, Texture2D<float2> cur, uint2 cell) {
    int2 origin = int2(cell) * BS;
    int2 hi     = g_grid - 1;
    int2 self   = to_px(cur.Load(int3(cell, 0)));
    float best  = motion_cost(mx, my, origin, self, self);
    int2  bv    = self;
    [loop] for (uint i = 0; i < 12; ++i) {
        int2  v = to_px(cur.Load(int3(clamp(int2(cell) + kNeighbours[i], 0, hi), 0)));
        float c = motion_cost(mx, my, origin, v, self);
        if (c < best) { best = c; bv = v; }
    }
    int2 base = bv;
    [loop] for (int dy = -1; dy <= 1; ++dy) {
        [loop] for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            int2  v = base + int2(dx, dy);
            float c = motion_cost(mx, my, origin, v, self);
            if (c < best) { best = c; bv = v; }
        }
    }
    float2 fv = (g_flags & FLAG_SUBPIXEL) ? subpixel(mx, my, origin, bv) : float2(bv);
    return fv * g_inv_msize;
}

[numthreads(8, 8, 1)]
void cs_motion_propagate(uint3 id : SV_DispatchThreadID) {
    if (int(id.x) >= g_grid.x || int(id.y) >= g_grid.y) return;
    if (id.z == 0)
        g_flow_out[id.xy] = propagate_block(g_ma, g_mb, g_coarse, id.xy);
    else
        g_flow_out1[id.xy] = propagate_block(g_mb, g_ma, g_coarse1, id.xy);
}
)HLSL";

// Médiane 3x3 par composante : supprime les vecteurs aberrants isolés.
// z = 0 : g_med_in -> g_med_out ; z = 1 : g_med_in1 -> g_med_out1 (les deux sens d'une paire).
inline constexpr char kMedian[] = R"HLSL(
Texture2D<float2>   g_med_in   : register(t0);
Texture2D<float2>   g_med_in1  : register(t1);
RWTexture2D<float2> g_med_out  : register(u0);
RWTexture2D<float2> g_med_out1 : register(u1);
cbuffer MedianCB : register(b0) {
    int2 g_mgrid;
    int2 g_medpad;
};

float median9(float v[9]) {
    [unroll] for (int i = 0; i < 5; ++i) {
        [unroll] for (int j = i + 1; j < 9; ++j) {
            float a = v[i];
            float b = v[j];
            v[i] = min(a, b);
            v[j] = max(a, b);
        }
    }
    return v[4];
}

float2 median3x3(Texture2D<float2> src, int2 c) {
    float xs[9];
    float ys[9];
    int k = 0;
    [unroll] for (int dy = -1; dy <= 1; ++dy) {
        [unroll] for (int dx = -1; dx <= 1; ++dx) {
            int2   p = clamp(c + int2(dx, dy), 0, g_mgrid - 1);
            float2 f = src.Load(int3(p, 0));
            xs[k] = f.x;
            ys[k] = f.y;
            ++k;
        }
    }
    return float2(median9(xs), median9(ys));
}

[numthreads(8, 8, 1)]
void cs_flow_median(uint3 id : SV_DispatchThreadID) {
    if (int(id.x) >= g_mgrid.x || int(id.y) >= g_mgrid.y) return;
    if (id.z == 0)
        g_med_out[id.xy] = median3x3(g_med_in, int2(id.xy));
    else
        g_med_out1[id.xy] = median3x3(g_med_in1, int2(id.xy));
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Synthèse d'une frame intermédiaire au temps t entre A (t=0) et B (t=1).
// Approximation linéaire du flow (Super SloMo) + gestion des occlusions et des éléments fixes (HUD).
inline constexpr char kSynth[] = R"HLSL(
Texture2D<float4> g_sa  : register(t0);
Texture2D<float4> g_sb  : register(t1);
Texture2D<float2> g_fab : register(t2);
Texture2D<float2> g_fba : register(t3);
Texture2D<float>  g_ea  : register(t4);  // masques de bords dilatés de A et B (cs_edge_*), 0 si absents
Texture2D<float>  g_eb  : register(t5);
cbuffer SynthCB : register(b0) {
    float  g_t;
    float  g_static_lo;
    float  g_static_hi;
    float  g_err_lo;
    float  g_err_hi;
    float2 g_flow_scale;  // UV image -> UV de la grille de flow (taille / (grille * 8))
    float  g_spad;
    float  g_hud_lo;      // près d'un bord : différence |A-B| tolérée pour garder le pixel fixe
    float  g_hud_hi;
    float  g_hud_on;      // 0 = protection du HUD désactivée
    float  g_spad2;
};

float4 ps_synth(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float  t  = g_t;
    float2 fab = g_fab.SampleLevel(g_linear, uv * g_flow_scale, 0);
    float2 fba = g_fba.SampleLevel(g_linear, uv * g_flow_scale, 0);
    float2 fta = -(1.0 - t) * t * fab + t * t * fba;
    float2 ftb = (1.0 - t) * (1.0 - t) * fab - t * (1.0 - t) * fba;

    float3 ca = g_sa.SampleLevel(g_linear, uv + fta, 0).rgb;
    float3 cb = g_sb.SampleLevel(g_linear, uv + ftb, 0).rgb;
    float3 sa = g_sa.SampleLevel(g_linear, uv, 0).rgb;
    float3 sb = g_sb.SampleLevel(g_linear, uv, 0).rgb;

    // Les deux échantillons compensés ne concordent pas : occlusion ou mauvais vecteur.
    float disagree = smoothstep(g_err_lo, g_err_hi, max3(abs(ca - cb)));
    float wb       = lerp(t, t < 0.5 ? 0.0 : 1.0, disagree);
    float3 mc      = lerp(ca, cb, wb);

    // Pixel identique dans A et B au même endroit (HUD, sous-titres, ciel) : on le garde fixe.
    // Différence mesurée sur 4 échantillons bilinéaires décalés (flou 3x3) pour ignorer le bruit
    // de compression.
    uint w, h;
    g_sa.GetDimensions(w, h);
    float2 o = 0.5 / float2(w, h);
    float3 d = abs(g_sa.SampleLevel(g_linear, uv + float2(-o.x, -o.y), 0).rgb - g_sb.SampleLevel(g_linear, uv + float2(-o.x, -o.y), 0).rgb)
             + abs(g_sa.SampleLevel(g_linear, uv + float2( o.x, -o.y), 0).rgb - g_sb.SampleLevel(g_linear, uv + float2( o.x, -o.y), 0).rgb)
             + abs(g_sa.SampleLevel(g_linear, uv + float2(-o.x,  o.y), 0).rgb - g_sb.SampleLevel(g_linear, uv + float2(-o.x,  o.y), 0).rgb)
             + abs(g_sa.SampleLevel(g_linear, uv + float2( o.x,  o.y), 0).rgb - g_sb.SampleLevel(g_linear, uv + float2( o.x,  o.y), 0).rgb);
    float dmax  = max3(d * 0.25);
    float still = 1.0 - smoothstep(g_static_lo, g_static_hi, dmax);
    // HUD / texte fixe : près d'un bord net (dans A ou B), le bruit de compression autour des traits
    // dépasse souvent le seuil « fixe » ci-dessus et le pixel serait entraîné par le mouvement du décor.
    // On y tolère une différence plus grande, pondérée par la stabilité entre A et B. Sur une zone unie,
    // seul le critère d'origine s'applique (un objet qui traverse un ciel uniforme reste interpolé).
    float edge  = max(g_ea.SampleLevel(g_linear, uv, 0), g_eb.SampleLevel(g_linear, uv, 0));
    float hud   = g_hud_on * edge * (1.0 - smoothstep(g_hud_lo, g_hud_hi, dmax));
    float keep  = max(still, hud);
    float3 c    = lerp(mc, lerp(sa, sb, t), keep);
    return float4(c, 1.0);
}
)HLSL";

// Visualisation du flow (teinte = direction, saturation = amplitude) sur l'image en gris.
inline constexpr char kFlowViz[] = R"HLSL(
Texture2D<float4> g_vimg  : register(t0);
Texture2D<float2> g_vflow : register(t1);
cbuffer VizCB : register(b0) {
    float  g_vscale;
    float  g_vpad;
    float2 g_vflow_scale;  // UV image -> UV de la grille de flow
};
float3 hue(float h) {
    return saturate(abs(fmod(h * 6.0 + float3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0);
}
float4 ps_flow_viz(VSOut i) : SV_Target {
    float2 f   = g_vflow.SampleLevel(g_linear, i.uv * g_vflow_scale, 0);
    float  mag = length(f) * g_vscale;
    float  ang = atan2(f.y, f.x) / 6.2831853 + 0.5;
    float  g   = luma(g_vimg.SampleLevel(g_linear, i.uv, 0).rgb);
    return float4(lerp(g.xxx * 0.6, hue(ang), saturate(mag)), 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Mire analytique (vérité terrain exacte à n'importe quel instant) : panoramique, disques
// en mouvement, HUD fixe et bruit façon compression.
inline constexpr char kTestPattern[] = R"HLSL(
cbuffer TestCB : register(b0) {
    float g_time;
    float g_noise;
    float g_seed;
    float g_speed;
};

float hash12(float2 p) {
    float3 p3 = frac(p.xyx * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 tp_background(float2 w) {
    float2 cell = floor(w / 64.0);
    float  chk  = abs(fmod(cell.x + cell.y, 2.0));
    float3 c    = lerp(float3(0.16, 0.22, 0.32), float3(0.78, 0.72, 0.58), chk);
    c *= 0.8 + 0.2 * sin(w.x * 0.045) * sin(w.y * 0.037);
    c *= 0.8 + 0.4 * hash12(floor(w / 8.0));
    return c;
}

float2 rot(float2 v, float a) {
    float s, c;
    sincos(a, s, c);
    return float2(c * v.x - s * v.y, s * v.x + c * v.y);
}

float4 ps_test_pattern(VSOut i) : SV_Target {
    float2 px = i.uv * float2(1920.0, 1080.0);
    float  t  = g_time * g_speed;

    float2 cam = float2(t * 420.0, sin(t * 0.7) * 80.0);
    float3 c   = tp_background(px + cam);

    float2 c0 = float2(960.0 + 560.0 * sin(t * 1.3), 540.0 + 240.0 * cos(t * 1.3));
    if (length(px - c0) < 110.0) {
        float2 l0 = rot(px - c0, t * 2.0);
        c = frac(l0.x / 22.0) < 0.5 ? float3(0.95, 0.55, 0.10) : float3(0.10, 0.30, 0.85);
    }

    float2 c1 = float2(fmod(t * 900.0, 2400.0) - 240.0, 300.0);
    float  d1 = length(px - c1);
    if (d1 < 75.0) c = frac(d1 / 14.0) < 0.5 ? float3(0.9, 0.9, 0.9) : float3(0.8, 0.1, 0.15);

    float2 c2 = float2(1400.0, 780.0 + 170.0 * sin(t * 2.1));
    float2 l2 = px - c2;
    if (length(l2) < 90.0) {
        float2 q = floor(l2 / 18.0);
        c = abs(fmod(q.x + q.y, 2.0)) > 0.5 ? float3(0.2, 0.85, 0.35) : float3(0.05, 0.1, 0.05);
    }

    if (px.x > 60.0 && px.x < 520.0 && px.y > 920.0 && px.y < 1020.0) {
        float2 h  = px - float2(60.0, 920.0);
        bool  bar = frac(h.x / 14.0) < 0.55 && frac(h.y / 25.0) < 0.6;
        c = bar ? float3(1.0, 1.0, 1.0) : float3(0.05, 0.05, 0.05);
    }

    if (g_noise > 0.0) c += (hash12(px * 1.37 + g_seed * 91.7) - 0.5) * g_noise;
    return float4(saturate(c), 1.0);
}
)HLSL";

// ---------------------------------------------------------------------------------------------
// Frame tap (format SmoovPlay v2) : NV12 -> BGRA. Y en R8_UNORM pleine résolution, Cb/Cr en R8G8_UNORM demi-
// résolution. Chrominance 4:2:0 interpolée, échantillons alignés à gauche horizontalement et centrés
// verticalement (placement par défaut de H.264/H.265). La matrice (BT.709/BT.601, plage limitée/complète)
// vient du CPU : rgb = g_row_* . (y, cb, cr) + g_row_*.w, avec y, cb, cr = octet / 255.
inline constexpr char kTapNv12[] = R"HLSL(
Texture2D<float>  g_tap_y  : register(t0);
Texture2D<float2> g_tap_uv : register(t1);
cbuffer TapCB : register(b0) {
    float4 g_row_r;
    float4 g_row_g;
    float4 g_row_b;
    float4 g_tap_size;  // xy : taille de Y, zw : taille de Cb/Cr
};

float4 ps_tap_nv12(VSOut i) : SV_Target {
    int2   p   = int2(i.pos.xy);
    float  y   = g_tap_y.Load(int3(p, 0));
    float2 cuv = float2(float(p.x) * 0.5 + 0.5, (float(p.y) + 0.5) * 0.5) / g_tap_size.zw;
    float2 c   = g_tap_uv.SampleLevel(g_linear, cuv, 0);
    float3 yuv = float3(y, c.x, c.y);
    float3 rgb = float3(dot(g_row_r.xyz, yuv) + g_row_r.w,
                        dot(g_row_g.xyz, yuv) + g_row_g.w,
                        dot(g_row_b.xyz, yuv) + g_row_b.w);
    return float4(saturate(rgb), 1.0);
}
)HLSL";

// Frame tap BGRA : copie dans la texture de sortie, alpha forcé à 1 (la spécification l'ignore).
inline constexpr char kTapBgra[] = R"HLSL(
Texture2D<float4> g_tap_bgra : register(t0);
float4 ps_tap_bgra(VSOut i) : SV_Target {
    return float4(g_tap_bgra.Load(int3(int2(i.pos.xy), 0)).rgb, 1.0);
}
)HLSL";

}  // namespace shaders
