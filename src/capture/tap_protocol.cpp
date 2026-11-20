// Protocole « frame tap » v2 de SmoovPlay : voir tap_protocol.hpp (spécification et émetteur de référence
// SmoovPlay, licence MIT, Copyright (c) 2026 the SmoovPlay authors ; THIRD-PARTY-NOTICES.md).
#include "tap_protocol.hpp"

#include <cmath>
#include <cstdio>

namespace tap {

uint64_t frame_bytes(uint32_t codec, uint32_t width, uint32_t height) {
    const uint64_t px = uint64_t(width) * height;
    if (codec == kBGRA) return px * 4;
    if (codec == kNV12) return px * 3 / 2;
    return 0;
}

Layout raw_layout(uint32_t codec, uint32_t width, uint32_t height) {
    const uint64_t frame = frame_bytes(codec, width, height);
    const uint64_t mib   = 1024 * 1024;
    Layout l;
    if (frame == 0) return l;
    const uint64_t slot = (kSlotHeader + frame + mib - 1) / mib * mib;
    if (slot > kSlotsBytes) return l;
    l.slot_size  = uint32_t(slot);
    l.slot_count = uint32_t(std::min<uint64_t>(kSlotsBytes / slot, kMaxSlots));
    return l;
}

const char* codec_name(uint32_t codec) {
    switch (codec) {
    case kH264: return "H.264";
    case kH265: return "H.265";
    case kH265Hdr10: return "H.265 HDR10";
    case kNV12: return "NV12";
    case kBGRA: return "BGRA";
    default: return "inconnu";
    }
}

uint32_t effective_colour(uint32_t colour, uint32_t height) {
    if (colour >= kBt709Limited && colour <= kBt601Full) return colour;
    return height >= 720 ? kBt709Limited : kBt601Limited;
}

const char* colour_name(uint32_t effective) {
    switch (effective) {
    case kBt709Limited: return "BT.709 plage limitée";
    case kBt709Full: return "BT.709 plage complète";
    case kBt601Limited: return "BT.601 plage limitée";
    case kBt601Full: return "BT.601 plage complète";
    default: return "?";
    }
}

namespace {
struct Coefs {
    double kr, kb;
    bool   full;
};
Coefs coefs(uint32_t effective) {
    const bool is709 = effective == kBt709Limited || effective == kBt709Full;
    const bool full  = effective == kBt709Full || effective == kBt601Full;
    return is709 ? Coefs{0.2126, 0.0722, full} : Coefs{0.299, 0.114, full};
}
}  // namespace

YuvToRgb yuv_to_rgb(uint32_t effective) {
    const Coefs  c  = coefs(effective);
    const double kg = 1.0 - c.kr - c.kb;
    // Y' = ay·y + by ; Pb = ac·cb + bc ; Pr = ac·cr + bc (y, cb, cr = octet / 255).
    const double ay = c.full ? 1.0 : 255.0 / 219.0, by = c.full ? 0.0 : -16.0 / 219.0;
    const double ac = c.full ? 1.0 : 255.0 / 224.0, bc = c.full ? -128.0 / 255.0 : -128.0 / 224.0;
    const double rv = 2.0 * (1.0 - c.kr), bu = 2.0 * (1.0 - c.kb);
    const double gu = 2.0 * c.kb * (1.0 - c.kb) / kg, gv = 2.0 * c.kr * (1.0 - c.kr) / kg;
    YuvToRgb m{};
    const double rows[3][4] = {
        {ay, 0.0, rv * ac, by + rv * bc},
        {ay, -gu * ac, -gv * ac, by - (gu + gv) * bc},
        {ay, bu * ac, 0.0, by + bu * bc},
    };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) m.m[i][j] = float(rows[i][j]);
    return m;
}

Yuv8 rgb_to_yuv(float r, float g, float b, uint32_t effective) {
    const Coefs  c  = coefs(effective);
    const double kg = 1.0 - c.kr - c.kb;
    const double y  = c.kr * r + kg * g + c.kb * b;
    const double pb = (b - y) / (2.0 * (1.0 - c.kb));
    const double pr = (r - y) / (2.0 * (1.0 - c.kr));
    auto q = [](double v) { return uint8_t(std::clamp(std::lround(v), 0L, 255L)); };
    if (c.full) return {q(255.0 * y), q(128.0 + 255.0 * pb), q(128.0 + 255.0 * pr)};
    return {q(16.0 + 219.0 * y), q(128.0 + 224.0 * pb), q(128.0 + 224.0 * pr)};
}

void yuv8_to_rgb8(const YuvToRgb& m, uint8_t y, uint8_t cb, uint8_t cr, uint8_t rgb[3]) {
    const float v[3] = {y / 255.0f, cb / 255.0f, cr / 255.0f};
    for (int i = 0; i < 3; ++i) {
        const float c = m.m[i][0] * v[0] + m.m[i][1] * v[1] + m.m[i][2] * v[2] + m.m[i][3];
        rgb[i]        = uint8_t(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
    }
}

bool SlotMeta::truncated() const { return (flags & kFlagTruncated) != 0 || copied < size; }

HeaderStatus check_header(Header& h, std::string* why, std::string* note) {
    char buf[512];
    auto set = [&](std::string* dst) {
        if (dst) *dst = buf;
    };
    if (note) note->clear();
    if (why) why->clear();
    if (h.magic != kMagic) return HeaderStatus::NoStream;
    if (h.version == 1) {
        std::snprintf(buf, sizeof(buf),
                      "frame tap version 1 (frames compressées %s, telles que les émet le patch frame tap de "
                      "chiaki-ng) : non prise en charge. rpenhance ne lit que la version 2 (frames décodées NV12 "
                      "ou BGRA) ; il faut un émetteur de frames décodées.",
                      codec_name(h.codec));
        set(why);
        return HeaderStatus::Unsupported;
    }
    if (h.version != 2) {
        std::snprintf(buf, sizeof(buf), "frame tap version %u inconnue (rpenhance lit la version 2)", h.version);
        set(why);
        return HeaderStatus::Unsupported;
    }
    if (h.codec != kNV12 && h.codec != kBGRA) {
        std::snprintf(buf, sizeof(buf), "frame tap version 2 avec un format %u (%s) : seuls NV12 (16) et BGRA (17) sont lus",
                      h.codec, codec_name(h.codec));
        set(why);
        return HeaderStatus::Unsupported;
    }
    if (h.width == 0 || h.height == 0 || h.width > 16384 || h.height > 16384 ||
        (h.codec == kNV12 && ((h.width | h.height) & 1u))) {
        std::snprintf(buf, sizeof(buf), "frame tap : taille d'image invalide %ux%u%s", h.width, h.height,
                      h.codec == kNV12 ? " (NV12 : largeur et hauteur paires)" : "");
        set(why);
        return HeaderStatus::Unsupported;
    }
    std::string warn;
    // Disposition annoncée inutilisable (ne tient pas dans 64 Mio, slots non alignés) : la spécification dit de
    // reprendre celle de la version 1 (32 slots de 2 Mio).
    const bool fits = h.slot_count >= 1 && h.slot_size >= kSlotHeader + 8 && h.slot_size % 8 == 0 &&
                      uint64_t(h.slot_count) * h.slot_size <= kSlotsBytes;
    if (!fits) {
        std::snprintf(buf, sizeof(buf),
                      "disposition annoncée inutilisable (%u slots de %u octets) : repli sur celle de la version 1 "
                      "(32 slots de 2 Mio). ",
                      h.slot_count, h.slot_size);
        warn += buf;
        h.slot_count = 32;
        h.slot_size  = 2u * 1024 * 1024;
    }
    if (frame_bytes(h.codec, h.width, h.height) > uint64_t(h.slot_size) - kSlotHeader) {
        std::snprintf(buf, sizeof(buf), "une frame %ux%u %s ne tient pas dans un slot de %u octets : frames tronquées. ",
                      h.width, h.height, codec_name(h.codec), h.slot_size);
        warn += buf;
    }
    if (h.codec == kNV12 && h.colour > kBt601Full) {
        std::snprintf(buf, sizeof(buf), "champ colour inconnu (%u) : traité comme auto. ", h.colour);
        warn += buf;
    }
    if (note) *note = warn;
    return HeaderStatus::Ok;
}

void Reader::start_session(const Header& h) {
    hdr_          = h;
    session_      = h.session;
    have_session_ = true;
    next_seq_     = 0;
    first_        = true;
    new_session_  = true;
    frame_bytes_  = frame_bytes(h.codec, h.width, h.height);
    ++sessions_;
}

Reader::State Reader::refresh() {
    const uint32_t m1 = a32(base_, off::magic).load(std::memory_order_acquire);
    if (m1 != kMagic) {  // pas de flux : pas encore commencé, en cours d'écriture de l'en-tête ou terminé
        state_ = seen_stream_ ? State::Ended : State::Waiting;
        return state_;
    }
    seen_stream_      = true;
    const uint32_t s1 = a32(base_, off::session).load(std::memory_order_acquire);
    // L'en-tête ne change pas au cours d'une session : rien à relire.
    if (have_session_ && s1 == session_ && (state_ == State::Streaming || state_ == State::Unsupported)) return state_;

    Header h;
    h.magic      = m1;
    h.version    = a32(base_, off::version).load(std::memory_order_relaxed);
    h.slot_count = a32(base_, off::slot_count).load(std::memory_order_relaxed);
    h.slot_size  = a32(base_, off::slot_size).load(std::memory_order_relaxed);
    h.codec      = a32(base_, off::codec).load(std::memory_order_relaxed);
    h.width      = a32(base_, off::width).load(std::memory_order_relaxed);
    h.height     = a32(base_, off::height).load(std::memory_order_relaxed);
    h.max_fps    = a32(base_, off::max_fps).load(std::memory_order_relaxed);
    h.qpc_freq   = a64(base_, off::qpc_freq).load(std::memory_order_relaxed);
    h.features   = a32(base_, off::features).load(std::memory_order_relaxed);
    h.colour     = a32(base_, off::colour).load(std::memory_order_relaxed);
    h.session    = s1;
    std::atomic_thread_fence(std::memory_order_acquire);
    // L'émetteur a recommencé l'en-tête pendant la lecture : on réessaiera au prochain appel.
    if (a32(base_, off::magic).load(std::memory_order_acquire) != m1 ||
        a32(base_, off::session).load(std::memory_order_acquire) != s1) {
        state_ = State::Waiting;
        return state_;
    }
    switch (check_header(h, &why_, &why_)) {
    case HeaderStatus::Ok:
        start_session(h);
        state_ = State::Streaming;
        break;
    case HeaderStatus::Unsupported:
        hdr_          = h;
        session_      = s1;
        have_session_ = true;
        state_        = State::Unsupported;
        break;
    case HeaderStatus::NoStream: state_ = State::Waiting; break;
    }
    return state_;
}

}  // namespace tap
