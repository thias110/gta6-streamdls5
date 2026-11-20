#include "audio_format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace audio_format {

namespace {
// Positions SPEAKER_* (ksmedia.h), recopiées pour rester sans en-tête Windows.
constexpr uint32_t kFL = 0x1, kFR = 0x2, kFC = 0x4, kLFE = 0x8, kBL = 0x10, kBR = 0x20, kFLC = 0x40, kFRC = 0x80,
                   kBC = 0x100, kSL = 0x200, kSR = 0x400, kTC = 0x800, kTFL = 0x1000, kTFC = 0x2000, kTFR = 0x4000,
                   kTBL = 0x8000, kTBC = 0x10000, kTBR = 0x20000;
constexpr float kM3dB = 0.70710678f;

// Gain (gauche, droite) d'une position vers la stéréo.
void stereo_gains(uint32_t pos, float& l, float& r) {
    switch (pos) {
    case kFL: case kFLC: l = 1.0f; r = 0.0f; break;
    case kFR: case kFRC: l = 0.0f; r = 1.0f; break;
    case kFC: l = r = kM3dB; break;
    case kLFE: l = r = 0.0f; break;
    case kBL: case kSL: case kTFL: l = kM3dB; r = 0.0f; break;
    case kBR: case kSR: case kTFR: l = 0.0f; r = kM3dB; break;
    case kTBL: l = 0.5f; r = 0.0f; break;
    case kTBR: l = 0.0f; r = 0.5f; break;
    case kBC: case kTC: case kTFC: case kTBC: l = r = 0.5f; break;
    default: l = r = 0.5f; break;  // position inconnue : au centre
    }
}

// Position (bit SPEAKER_*) du i-ème canal : i-ème bit à 1 du masque, 0 si le masque n'en a pas assez.
uint32_t position_of(uint32_t mask, uint16_t i) {
    uint16_t k = 0;
    for (uint32_t bit = 1; bit; bit <<= 1)
        if (mask & bit) {
            if (k == i) return bit;
            ++k;
        }
    return 0;
}
}  // namespace

const char* sample_name(Sample s) {
    switch (s) {
    case Sample::Int16: return "int16";
    case Sample::Int24: return "int24";
    case Sample::Int32: return "int32";
    case Sample::Float32: return "float32";
    default: return "inconnu";
    }
}

uint32_t sample_bytes(Sample s) {
    switch (s) {
    case Sample::Int16: return 2;
    case Sample::Int24: return 3;
    case Sample::Int32:
    case Sample::Float32: return 4;
    default: return 0;
    }
}

std::string describe(const Pcm& p) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%u Hz, %u canaux, %s", p.rate, unsigned(p.channels), sample_name(p.sample));
    return buf;
}

uint32_t default_mask(uint16_t channels) {
    switch (channels) {
    case 1: return kFC;
    case 2: return kFL | kFR;
    case 3: return kFL | kFR | kFC;
    case 4: return kFL | kFR | kBL | kBR;
    case 5: return kFL | kFR | kFC | kBL | kBR;
    case 6: return kFL | kFR | kFC | kLFE | kBL | kBR;
    case 7: return kFL | kFR | kFC | kLFE | kBL | kBR | kBC;
    case 8: return kFL | kFR | kFC | kLFE | kBL | kBR | kSL | kSR;
    default: return 0;
    }
}

std::vector<float> mix_matrix(uint16_t src, uint32_t src_mask, uint16_t dst) {
    std::vector<float> m(size_t(src) * dst, 0.0f);
    if (!src || !dst) return m;
    auto at = [&](uint16_t d, uint16_t s) -> float& { return m[size_t(d) * src + s]; };
    if (src == dst) {
        for (uint16_t i = 0; i < src; ++i) at(i, i) = 1.0f;
        return m;
    }
    if (src == 1) {  // mono -> tous les canaux avant (stéréo : les deux)
        for (uint16_t d = 0; d < std::min<uint16_t>(dst, 2); ++d) at(d, 0) = 1.0f;
        return m;
    }
    const uint32_t mask = src_mask ? src_mask : default_mask(src);
    if (dst == 1) {
        uint16_t n = 0;
        for (uint16_t s = 0; s < src; ++s) n += position_of(mask, s) != kLFE;
        for (uint16_t s = 0; s < src; ++s)
            if (position_of(mask, s) != kLFE) at(0, s) = 1.0f / float(n ? n : 1);
        return m;
    }
    if (dst == 2) {
        for (uint16_t s = 0; s < src; ++s) {
            uint32_t pos = position_of(mask, s);
            if (!pos) pos = s == 0 ? kFL : s == 1 ? kFR : 0;  // masque incomplet : 2 premiers = avant G/D
            float l = 0.0f, r = 0.0f;
            stereo_gains(pos, l, r);
            at(0, s) = l;
            at(1, s) = r;
        }
        for (uint16_t d = 0; d < 2; ++d) {  // normalisation : jamais plus que 0 dBFS en sortie
            float sum = 0.0f;
            for (uint16_t s = 0; s < src; ++s) sum += at(d, s);
            if (sum > 1.0f)
                for (uint16_t s = 0; s < src; ++s) at(d, s) /= sum;
        }
        return m;
    }
    for (uint16_t i = 0; i < std::min(src, dst); ++i) at(i, i) = 1.0f;  // autres cas : canaux communs
    return m;
}

bool is_identity(const std::vector<float>& m, uint16_t src, uint16_t dst) {
    if (src != dst || m.size() != size_t(src) * dst) return false;
    for (uint16_t d = 0; d < dst; ++d)
        for (uint16_t s = 0; s < src; ++s)
            if (m[size_t(d) * src + s] != (d == s ? 1.0f : 0.0f)) return false;
    return true;
}

namespace {
void decode(const uint8_t* src, size_t n, Sample k, float* dst) {
    switch (k) {
    case Sample::Float32: std::memcpy(dst, src, n * sizeof(float)); break;
    case Sample::Int16:
        for (size_t i = 0; i < n; ++i) {
            int16_t v;
            std::memcpy(&v, src + i * 2, 2);
            dst[i] = float(v) * (1.0f / 32768.0f);
        }
        break;
    case Sample::Int24:
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* p = src + i * 3;
            const int32_t  v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8;
            dst[i]           = float(v) * (1.0f / 8388608.0f);
        }
        break;
    case Sample::Int32:
        for (size_t i = 0; i < n; ++i) {
            int32_t v;
            std::memcpy(&v, src + i * 4, 4);
            dst[i] = float(double(v) * (1.0 / 2147483648.0));
        }
        break;
    default: std::fill(dst, dst + n, 0.0f); break;
    }
}
}  // namespace

void to_float(const uint8_t* src, uint32_t frames, const Pcm& in, uint16_t dst_ch, const std::vector<float>& matrix,
              float* dst, std::vector<float>& scratch) {
    const uint16_t sc = in.channels;
    if (is_identity(matrix, sc, dst_ch)) {
        decode(src, size_t(frames) * sc, in.sample, dst);
        return;
    }
    scratch.resize(size_t(frames) * sc);
    decode(src, scratch.size(), in.sample, scratch.data());
    for (uint32_t f = 0; f < frames; ++f) {
        const float* s = scratch.data() + size_t(f) * sc;
        float*       o = dst + size_t(f) * dst_ch;
        for (uint16_t d = 0; d < dst_ch; ++d) {
            const float* row = matrix.data() + size_t(d) * sc;
            float        acc = 0.0f;
            for (uint16_t c = 0; c < sc; ++c) acc += row[c] * s[c];
            o[d] = acc;
        }
    }
}

void from_float(const float* src, uint32_t frames, const Pcm& out, uint8_t* dst) {
    const size_t n = size_t(frames) * out.channels;
    switch (out.sample) {
    case Sample::Float32: std::memcpy(dst, src, n * sizeof(float)); break;
    case Sample::Int16:
        for (size_t i = 0; i < n; ++i) {
            const int16_t v = int16_t(std::lrint(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f));
            std::memcpy(dst + i * 2, &v, 2);
        }
        break;
    case Sample::Int24:
        for (size_t i = 0; i < n; ++i) {
            const int32_t v = int32_t(std::lrint(double(std::clamp(src[i], -1.0f, 1.0f)) * 8388607.0));
            dst[i * 3]      = uint8_t(v);
            dst[i * 3 + 1]  = uint8_t(v >> 8);
            dst[i * 3 + 2]  = uint8_t(v >> 16);
        }
        break;
    case Sample::Int32:
        for (size_t i = 0; i < n; ++i) {
            const int32_t v = int32_t(std::llrint(double(std::clamp(src[i], -1.0f, 1.0f)) * 2147483647.0));
            std::memcpy(dst + i * 4, &v, 4);
        }
        break;
    default: std::memset(dst, 0, n * std::max<uint32_t>(sample_bytes(out.sample), 1)); break;
    }
}

std::vector<Candidate> output_candidates(const Pcm& in, const Pcm& out_mix) {
    std::vector<Candidate> c;
    auto add = [&](uint32_t rate, uint16_t ch, Sample s, uint32_t mask, bool autoconvert) {
        if (!rate || !ch || s == Sample::Unknown) return;
        Candidate x{{rate, ch, s, mask ? mask : default_mask(ch)}, autoconvert};
        for (const auto& y : c)
            if (y.fmt.rate == rate && y.fmt.channels == ch && y.fmt.sample == s && y.autoconvert == autoconvert) return;
        c.push_back(x);
    };
    // Mêmes canaux que l'entrée si la sortie a les mêmes (ou si l'entrée est mono/stéréo), sinon stéréo.
    const uint16_t ch   = (in.channels <= 2 || in.channels == out_mix.channels) ? in.channels : 2;
    const uint32_t mask = ch == in.channels ? in.mask : 0;
    add(in.rate, ch, Sample::Float32, mask, true);
    add(in.rate, ch, Sample::Int16, mask, true);
    add(in.rate, 2, Sample::Float32, 0, true);
    add(in.rate, 2, Sample::Int16, 0, true);
    if (out_mix.valid()) add(out_mix.rate, out_mix.channels, out_mix.sample, out_mix.mask, false);
    return c;
}

}  // namespace audio_format
