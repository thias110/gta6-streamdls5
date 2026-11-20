// Tests du lecteur « frame tap » v2 (logique CPU pure, sans Windows ni GPU) sur un faux segment en mémoire.
// L'émetteur simulé suit les étapes de la spécification MIT de SmoovPlay (docs/frame-tap.md, Copyright (c)
// 2026 the SmoovPlay authors ; voir THIRD-PARTY-NOTICES.md).
// Compilation autonome : g++ -std=c++20 -Isrc tests/tap_protocol_test.cpp src/capture/tap_protocol.cpp
#include "capture/tap_pattern.hpp"
#include "capture/tap_protocol.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {
int g_failures = 0;

void expect(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "OK   " : "ÉCHEC", what);
    if (!ok) ++g_failures;
}

// Faux segment partagé (aligné sur 8) et émetteur conforme aux étapes de la spécification.
struct Segment {
    std::unique_ptr<uint64_t[]> mem{new uint64_t[tap::kTotalBytes / 8]()};
    uint8_t*                    base() { return reinterpret_cast<uint8_t*>(mem.get()); }
    uint32_t&                   u32(size_t o) { return *reinterpret_cast<uint32_t*>(base() + o); }
    uint64_t&                   u64(size_t o) { return *reinterpret_cast<uint64_t*>(base() + o); }
};

struct FakeWriter {
    Segment& s;
    uint32_t slot_count = 0, slot_size = 0;

    void begin(uint32_t version, uint32_t codec, uint32_t w, uint32_t h, uint32_t colour = 0,
               const tap::Layout* forced = nullptr) {
        const tap::Layout l = forced ? *forced : tap::raw_layout(codec, w, h);
        slot_count          = l.slot_count;
        slot_size           = l.slot_size;
        s.u32(tap::off::magic)      = 0;
        s.u32(tap::off::version)    = version;
        s.u32(tap::off::slot_count) = slot_count;
        s.u32(tap::off::slot_size)  = slot_size;
        s.u32(tap::off::codec)      = codec;
        s.u32(tap::off::width)      = w;
        s.u32(tap::off::height)     = h;
        s.u32(tap::off::max_fps)    = 60;
        s.u64(tap::off::qpc_freq)   = 10000000;
        s.u32(tap::off::colour)     = colour;
        s.u64(tap::off::write_seq)  = 0;
        s.u32(tap::off::session) += 1;
        s.u32(tap::off::magic) = tap::kMagic;
    }
    uint8_t* slot(uint64_t n) { return s.base() + tap::kHeaderBytes + size_t(n % slot_count) * slot_size; }
    // Écrit la frame n = write_seq ; payload : octets de données (le premier octet vaut n & 0xFF).
    void push(uint32_t size, uint32_t flags, int32_t lost = 0, uint32_t recovered = 0) {
        const uint64_t n  = s.u64(tap::off::write_seq);
        uint8_t*       sl = slot(n);
        *reinterpret_cast<uint64_t*>(sl + 0)  = tap::kBusy;
        *reinterpret_cast<uint64_t*>(sl + 8)  = 1000 + n;
        *reinterpret_cast<uint32_t*>(sl + 16) = size;
        *reinterpret_cast<int32_t*>(sl + 20)  = lost;
        *reinterpret_cast<uint32_t*>(sl + 24) = recovered;
        *reinterpret_cast<uint32_t*>(sl + 28) = flags;
        const uint32_t room = slot_size - tap::kSlotHeader;
        std::memset(sl + 64, int(n & 0xFF), std::min(size, room));
        *reinterpret_cast<uint64_t*>(sl + 0) = n;
        s.u64(tap::off::write_seq)           = n + 1;
    }
    void end() { s.u32(tap::off::magic) = 0; }
};

}  // namespace

int main() {
    using tap::Reader;
    {  // 1. Dispositions de la spécification (version 2).
        const auto b1080 = tap::raw_layout(tap::kBGRA, 1920, 1080), n1080 = tap::raw_layout(tap::kNV12, 1920, 1080),
                   b1440 = tap::raw_layout(tap::kBGRA, 2560, 1440), b4k = tap::raw_layout(tap::kBGRA, 3840, 2160);
        expect(b1080.slot_count == 8 && b1080.slot_size == 8u << 20, "1080p BGRA : 8 slots de 8 Mio");
        expect(n1080.slot_count == 21 && n1080.slot_size == 3u << 20, "1080p NV12 : 21 slots de 3 Mio");
        expect(b1440.slot_count == 4 && b1440.slot_size == 15u << 20, "1440p BGRA : 4 slots de 15 Mio");
        expect(!b4k.ok(), "4K BGRA : ne tient pas");
        expect(tap::kTotalBytes == 67112960ull, "taille du segment 4096 + 64 Mio = 67 112 960");
        const auto small = tap::raw_layout(tap::kNV12, 640, 360);
        expect(small.slot_count == 32 && small.slot_size == 1u << 20, "petites frames : 32 slots au plus");
    }

    Segment    seg;
    FakeWriter w{seg};
    Reader     r(seg.base());
    tap::SlotMeta m;
    std::vector<uint8_t> buf;
    auto copy_to_buf = [&](const uint8_t* d, size_t n) { buf.assign(d, d + n); };

    {  // 2. Rien d'écrit : attente, pas de frame ; le lecteur écrit son heartbeat.
        expect(r.refresh() == Reader::State::Waiting, "segment vide : en attente");
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::None, "segment vide : aucune frame");
        r.heartbeat(123456);
        expect(seg.u64(tap::off::reader_heartbeat) == 123456, "reader_heartbeat écrit à l'offset 56");
    }
    {  // 3. Version 1 (frames compressées) : refus explicite.
        w.begin(1, tap::kH264, 1920, 1080, 0, nullptr);
        seg.u32(tap::off::slot_count) = 32;
        seg.u32(tap::off::slot_size)  = 2u << 20;
        expect(r.refresh() == Reader::State::Unsupported, "version 1 : non prise en charge");
        expect(r.reason().find("version 1") != std::string::npos && r.reason().find("compress") != std::string::npos,
               "version 1 : message clair (version 1, compressées)");
        std::printf("       message : %s\n", r.reason().c_str());
        w.end();
        expect(r.refresh() == Reader::State::Ended, "fin du flux v1 : terminé");
    }
    {  // 4. Version 2 NV12 : lecture de la plus récente, sauts comptés, rien de nouveau.
        w.begin(2, tap::kNV12, 1280, 720);
        expect(r.refresh() == Reader::State::Streaming, "version 2 NV12 : flux accepté");
        expect(r.header().slot_count == 32 && r.header().slot_size == 2u << 20, "720p NV12 : 32 slots de 2 Mio");
        const uint32_t fsz = 1280 * 720 * 3 / 2;
        for (int i = 0; i < 3; ++i) w.push(fsz, i % 2 ? tap::kFlagRepeat : tap::kFlagNew);
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::Frame && m.seq == 2, "la plus récente (n = 2) est lue");
        expect(buf.size() == fsz && buf[0] == 2 && buf[fsz - 1] == 2, "données de la frame 2 copiées en entier");
        expect(m.qpc_arrival == 1002 && m.flags == tap::kFlagNew && !m.truncated(), "métadonnées du slot relues");
        expect(r.take_new_session() && !r.take_new_session(), "nouvelle session signalée une seule fois");
        expect(r.skipped() == 0, "première lecture d'une session : pas de saut compté");
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::None, "rien de nouveau : aucune frame");
        for (int i = 0; i < 5; ++i) w.push(fsz, 0, i == 4 ? 3 : 0, i == 4 ? 1 : 0);
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::Frame && m.seq == 7, "en retard : saute à n = 7");
        expect(r.skipped() == 4, "frames 3 à 6 comptées comme sautées");
        expect(m.frames_lost == 3 && m.recovered == 1, "frames_lost et recovered relus");
    }
    {  // 5. Slot réécrit pendant la copie : abandonné ; slot déjà repris : abandonné.
        const uint32_t fsz = 1280 * 720 * 3 / 2;
        w.push(fsz, tap::kFlagNew);  // n = 8
        const auto dropped0 = r.dropped();
        auto       racing   = [&](const uint8_t* d, size_t n) {
            buf.assign(d, d + n);
            *reinterpret_cast<uint64_t*>(w.slot(8)) = tap::kBusy;  // l'émetteur reprend le slot pendant la copie
        };
        expect(r.read_latest(m, racing) == Reader::Read::Dropped, "slot réécrit pendant la copie : frame abandonnée");
        expect(r.dropped() == dropped0 + 1, "abandon compté");
        *reinterpret_cast<uint64_t*>(w.slot(8)) = 8;
        w.push(fsz, tap::kFlagNew);                                        // n = 9
        *reinterpret_cast<uint64_t*>(w.slot(9)) = 9 + w.slot_count;        // déjà rattrapé par l'émetteur
        bool called = false;
        expect(r.read_latest(m, [&](const uint8_t*, size_t) { called = true; }) == Reader::Read::Dropped && !called,
               "slot déjà repris : abandonné sans copie");
        w.push(fsz, tap::kFlagRepeat);  // n = 10
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::Frame && m.seq == 10 && buf[0] == 10,
               "la frame suivante est lue normalement");
    }
    {  // 6. Taille annoncée plus grande que le slot / frame tronquée.
        w.push(5u << 20, tap::kFlagNew);  // n = 11 : 5 Mio annoncés dans un slot de 2 Mio
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::Frame && buf.size() == 1280u * 720 * 3 / 2,
               "taille bornée à une frame (jamais au-delà du slot)");
        w.push(1000, tap::kFlagNew | tap::kFlagTruncated);  // n = 12
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::Frame && buf.size() == 1000 && m.truncated(),
               "frame tronquée : taille et drapeau");
    }
    {  // 7. Arrêt puis redémarrage de l'émetteur : nouvelle session, numéros repartis de zéro.
        const uint32_t s0 = r.header().session;
        w.end();
        expect(r.refresh() == Reader::State::Ended, "magic à 0 : flux terminé");
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::None, "flux terminé : aucune frame");
        w.begin(2, tap::kBGRA, 1920, 1080);
        expect(r.refresh() == Reader::State::Streaming && r.header().session == s0 + 1, "redémarrage : session suivante");
        expect(r.header().slot_count == 8 && r.header().codec == tap::kBGRA, "nouvelle disposition relue (BGRA 1080p)");
        w.push(1920 * 1080 * 4, tap::kFlagNew);
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::Frame && m.seq == 0 && r.take_new_session(),
               "première frame de la nouvelle session (n = 0)");
        // write_seq remis à zéro sans changer de session : le lecteur repart aussi.
        for (int i = 0; i < 3; ++i) w.push(1920 * 1080 * 4, 0);
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::Frame && m.seq == 3, "n = 3");
        seg.u64(tap::off::write_seq) = 0;
        w.push(1920 * 1080 * 4, 0);
        expect(r.read_latest(m, copy_to_buf) == Reader::Read::Frame && m.seq == 0 && r.take_new_session(),
               "write_seq redémarré : relu depuis 0");
    }
    {  // 8. En-têtes invalides et disposition de repli.
        tap::Header h;
        h.magic = tap::kMagic, h.version = 2, h.codec = tap::kNV12, h.width = 1919, h.height = 1080;
        h.slot_count = 21, h.slot_size = 3u << 20;
        std::string why, note;
        expect(tap::check_header(h, &why, &note) == tap::HeaderStatus::Unsupported, "NV12 de largeur impaire refusé");
        h.width = 1920, h.codec = 99;
        expect(tap::check_header(h, &why, &note) == tap::HeaderStatus::Unsupported, "format inconnu refusé");
        h.codec = tap::kNV12, h.version = 3;
        expect(tap::check_header(h, &why, &note) == tap::HeaderStatus::Unsupported, "version 3 refusée");
        h.version = 2, h.slot_count = 32, h.slot_size = 3u << 20;  // 96 Mio : ne tient pas
        expect(tap::check_header(h, &why, &note) == tap::HeaderStatus::Ok && h.slot_count == 32 &&
                   h.slot_size == 2u << 20 && !note.empty(),
               "disposition trop grande : repli sur 32 x 2 Mio (avec avertissement)");
        h.magic = 0;
        expect(tap::check_header(h, &why, &note) == tap::HeaderStatus::NoStream, "magic à 0 : pas de flux");
    }
    {  // 9. Couleur : choix auto, aller-retour RGB -> YUV -> RGB et valeurs de référence.
        expect(tap::effective_colour(0, 720) == tap::kBt709Limited && tap::effective_colour(0, 719) == tap::kBt601Limited,
               "auto : BT.709 à partir de 720 lignes, BT.601 en dessous");
        expect(tap::effective_colour(4, 1080) == tap::kBt601Full && tap::effective_colour(9, 1080) == tap::kBt709Limited,
               "valeurs explicites gardées, inconnue = auto");
        int worst = 0;
        for (uint32_t c = 1; c <= 4; ++c) {
            const auto mtx = tap::yuv_to_rgb(c);
            for (const auto& p : tap_pattern::kPatches) {
                const auto v = tap::rgb_to_yuv(p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, c);
                uint8_t    o[3];
                tap::yuv8_to_rgb8(mtx, v.y, v.cb, v.cr, o);
                worst = std::max({worst, std::abs(o[0] - p.r), std::abs(o[1] - p.g), std::abs(o[2] - p.b)});
            }
        }
        std::printf("       écart max aller-retour : %d\n", worst);
        expect(worst <= 2, "aller-retour RGB -> YUV 8 bits -> RGB : écart <= 2 sur les 4 matrices");
        uint8_t o[3];
        tap::yuv8_to_rgb8(tap::yuv_to_rgb(tap::kBt709Limited), 16, 128, 128, o);
        expect(o[0] == 0 && o[1] == 0 && o[2] == 0, "plage limitée : Y = 16 -> noir");
        tap::yuv8_to_rgb8(tap::yuv_to_rgb(tap::kBt709Limited), 235, 128, 128, o);
        expect(o[0] == 255 && o[1] == 255 && o[2] == 255, "plage limitée : Y = 235 -> blanc");
        // Une mauvaise matrice se voit : rouge BT.709 décodé en BT.601.
        const auto v = tap::rgb_to_yuv(200 / 255.0f, 40 / 255.0f, 40 / 255.0f, tap::kBt709Limited);
        tap::yuv8_to_rgb8(tap::yuv_to_rgb(tap::kBt601Limited), v.y, v.cb, v.cr, o);
        expect(std::abs(o[0] - 200) + std::abs(o[1] - 40) + std::abs(o[2] - 40) > 12, "BT.709 lu en BT.601 : écart visible");
    }
    {  // 10. Image de test : numéro de frame relu en BGRA et après NV12 (conversion CPU de référence).
        const uint32_t W = 800, H = 160;
        std::vector<uint8_t> bgra(size_t(W) * H * 4), nv12(size_t(W) * H * 3 / 2), back(size_t(W) * H * 4);
        tap_pattern::fill_bgra(bgra.data(), W, H, 0xA5C3, 7);
        expect(tap_pattern::decode_frame(bgra.data(), W * 4) == 0xA5C3, "numéro relu dans l'image BGRA");
        const uint32_t eff = tap::effective_colour(0, H);
        tap_pattern::fill_nv12(nv12.data(), W, H, 0x5A3C, 7, eff);
        const auto mtx = tap::yuv_to_rgb(eff);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const uint8_t* uv = nv12.data() + size_t(W) * H + size_t(y / 2) * W + (x / 2) * 2;
                uint8_t        o[3];
                tap::yuv8_to_rgb8(mtx, nv12[size_t(y) * W + x], uv[0], uv[1], o);
                uint8_t* p = back.data() + (size_t(y) * W + x) * 4;
                p[0] = o[2], p[1] = o[1], p[2] = o[0], p[3] = 255;
            }
        expect(tap_pattern::decode_frame(back.data(), W * 4) == 0x5A3C, "numéro relu après NV12 -> RGB");
        int worst = 0;
        for (uint32_t k = 0; k < tap_pattern::kPatchCount; ++k) {
            const uint8_t* p = back.data() + (size_t(tap_pattern::patch_cy()) * W + tap_pattern::patch_cx(k)) * 4;
            const auto&    e = tap_pattern::kPatches[k];
            worst = std::max({worst, std::abs(p[2] - e.r), std::abs(p[1] - e.g), std::abs(p[0] - e.b)});
        }
        std::printf("       écart max des pastilles après NV12 : %d\n", worst);
        expect(worst <= 3, "pastilles de couleur après NV12 -> RGB : écart <= 3");
    }

    std::printf(g_failures ? "\n%d ÉCHEC(S)\n" : "\nTOUT OK\n", g_failures);
    return g_failures ? 1 : 0;
}
