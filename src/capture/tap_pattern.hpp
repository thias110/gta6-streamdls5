#pragma once
// Image de test du frame tap, partagée par l'émetteur de test (tests/tap_writer.cpp) et le contrôle
// `rpenhance --selftest-tap` : chaque frame porte son numéro (16 blocs noirs/blancs en haut à gauche, bit 0 à
// gauche), 8 pastilles de couleurs connues pour mesurer l'écart de la conversion NV12 -> BGRA, et un fond de
// bandes mobiles repris de l'émetteur de référence MIT de SmoovPlay (docs/frame-tap-writer.c, Copyright (c)
// 2026 the SmoovPlay authors) : B = x + image * 8, G = y / 4, R = numéro de l'image.
// Sans dépendance Windows.

#include "tap_protocol.hpp"

#include <cstdint>
#include <vector>

namespace tap_pattern {

inline constexpr uint32_t kBits       = 16;  // numéro de frame modulo 65536
inline constexpr uint32_t kBlock      = 32;  // côté d'un bloc de bit (pixels)
inline constexpr uint32_t kPatchX     = 16;
inline constexpr uint32_t kPatchY     = 64;
inline constexpr uint32_t kPatchSize  = 64;
inline constexpr uint32_t kPatchStep  = 96;
inline constexpr uint32_t kPatchCount = 8;
inline constexpr uint32_t kMinWidth   = kPatchX + kPatchCount * kPatchStep;  // 784
inline constexpr uint32_t kMinHeight  = kPatchY + kPatchSize + 16;          // 144

struct Rgb {
    uint8_t r, g, b;
};
inline constexpr Rgb kPatches[kPatchCount] = {
    {200, 40, 40}, {40, 180, 60}, {50, 60, 210}, {128, 128, 128},
    {235, 200, 60}, {30, 30, 30}, {220, 220, 220}, {180, 60, 170},
};

// Centre de la pastille k (pixels).
inline uint32_t patch_cx(uint32_t k) { return kPatchX + k * kPatchStep + kPatchSize / 2; }
inline uint32_t patch_cy() { return kPatchY + kPatchSize / 2; }
// Centre du bloc du bit b.
inline uint32_t bit_cx(uint32_t b) { return b * kBlock + kBlock / 2; }
inline uint32_t bit_cy() { return kBlock / 2; }

inline Rgb pixel(uint32_t x, uint32_t y, uint32_t frame, uint32_t picture) {
    if (y < kBlock && x < kBits * kBlock) {
        const uint8_t v = ((frame >> (x / kBlock)) & 1u) ? 255 : 0;
        return {v, v, v};
    }
    if (y >= kPatchY && y < kPatchY + kPatchSize && x >= kPatchX) {
        const uint32_t k = (x - kPatchX) / kPatchStep, dx = (x - kPatchX) % kPatchStep;
        if (k < kPatchCount && dx < kPatchSize) return kPatches[k];
    }
    return {uint8_t(picture), uint8_t(y / 4), uint8_t(x + picture * 8)};
}

// BGRA, lignes tassées (w * 4 octets).
inline void fill_bgra(uint8_t* dst, uint32_t w, uint32_t h, uint32_t frame, uint32_t picture) {
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const Rgb c = pixel(x, y, frame, picture);
            uint8_t*  p = dst + (size_t(y) * w + x) * 4;
            p[0] = c.b;
            p[1] = c.g;
            p[2] = c.r;
            p[3] = 255;
        }
}

// NV12, lignes tassées : w * h octets de Y puis w * h / 2 de Cb, Cr entrelacés (w et h pairs). La chrominance
// d'un bloc 2x2 est celle de sa couleur moyenne.
inline void fill_nv12(uint8_t* dst, uint32_t w, uint32_t h, uint32_t frame, uint32_t picture, uint32_t effective) {
    uint8_t* uv = dst + size_t(w) * h;
    for (uint32_t y = 0; y < h; y += 2)
        for (uint32_t x = 0; x < w; x += 2) {
            float sr = 0, sg = 0, sb = 0;
            for (uint32_t k = 0; k < 4; ++k) {
                const uint32_t px = x + (k & 1u), py = y + (k >> 1);
                const Rgb      c  = pixel(px, py, frame, picture);
                const tap::Yuv8 v = tap::rgb_to_yuv(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, effective);
                dst[size_t(py) * w + px] = v.y;
                sr += c.r;
                sg += c.g;
                sb += c.b;
            }
            const tap::Yuv8 v = tap::rgb_to_yuv(sr / 1020.0f, sg / 1020.0f, sb / 1020.0f, effective);
            uint8_t*        p = uv + size_t(y / 2) * w + x;
            p[0] = v.cb;
            p[1] = v.cr;
        }
}

// Relit le numéro de frame dans une image BGRA (pitch en octets).
inline uint32_t decode_frame(const uint8_t* bgra, size_t pitch) {
    uint32_t v = 0;
    for (uint32_t b = 0; b < kBits; ++b) {
        const uint8_t* p = bgra + size_t(bit_cy()) * pitch + size_t(bit_cx(b)) * 4;
        if (p[1] >= 128) v |= 1u << b;
    }
    return v;
}

}  // namespace tap_pattern
