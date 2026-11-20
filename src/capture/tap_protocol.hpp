#pragma once
// Protocole « frame tap » de SmoovPlay, version 2 : frames décodées (NV12 ou BGRA) déposées par une autre
// appli dans une mémoire partagée Windows. Lecteur écrit pour rpenhance d'après la spécification MIT de
// SmoovPlay (docs/frame-tap.md) et son émetteur de référence MIT (docs/frame-tap-writer.c),
// https://github.com/smoovplay/smoovplay — Copyright (c) 2026 the SmoovPlay authors, licence MIT (texte
// complet dans THIRD-PARTY-NOTICES.md). Aucun code de chiaki-ng (AGPL) n'a été lu ni repris.
//
// Ce fichier ne dépend pas de Windows : la logique (en-tête, disposition des slots, lecture sans bloquer
// l'émetteur, sessions) est testée sur un faux segment en mémoire (tests/tap_protocol_test.cpp).

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace tap {

inline constexpr uint32_t kMagic       = 0x50544643u;  // 'CFTP'
inline constexpr uint32_t kHeaderBytes = 4096;
inline constexpr uint64_t kSlotsBytes  = 64ull * 1024 * 1024;
inline constexpr uint64_t kTotalBytes  = kHeaderBytes + kSlotsBytes;  // 67 112 960 octets, toujours
inline constexpr uint32_t kSlotHeader  = 64;
inline constexpr uint32_t kMaxSlots    = 32;
inline constexpr uint32_t kMinSlots    = 4;
inline constexpr uint64_t kBusy        = ~0ull;  // `seq` d'un slot en cours de réécriture
inline constexpr const wchar_t* kDefaultName = L"Local\\smoovplay-tap";

enum Codec : uint32_t { kH264 = 0, kH265 = 1, kH265Hdr10 = 2, kNV12 = 16, kBGRA = 17 };
enum SlotFlag : uint32_t { kFlagTruncated = 1, kFlagRepeat = 2, kFlagNew = 4 };
enum Colour : uint32_t { kColourAuto = 0, kBt709Limited = 1, kBt709Full = 2, kBt601Limited = 3, kBt601Full = 4 };

// Décalages (octets) dans l'en-tête et dans l'en-tête de slot. Tout est little-endian.
namespace off {
inline constexpr size_t magic = 0, version = 4, slot_count = 8, slot_size = 12, codec = 16, width = 20, height = 24,
                        max_fps = 28, write_seq = 32, idr_request = 40, session = 44, qpc_freq = 48,
                        reader_heartbeat = 56, features = 64, audio_delay_ms = 88, colour = 92;
namespace slot {
inline constexpr size_t seq = 0, qpc_arrival = 8, size = 16, frames_lost = 20, recovered = 24, flags = 28;
}
}  // namespace off

// Accès atomiques à la mémoire partagée (les champs u32/u64 sont alignés sur leur taille).
inline std::atomic_ref<uint32_t> a32(uint8_t* base, size_t o) { return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(base + o)); }
inline std::atomic_ref<uint64_t> a64(uint8_t* base, size_t o) { return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(base + o)); }

struct Header {
    uint32_t magic = 0, version = 0, slot_count = 0, slot_size = 0, codec = 0, width = 0, height = 0, max_fps = 0;
    uint32_t session = 0, features = 0, colour = 0;
    uint64_t qpc_freq = 0;
};

// Disposition des slots en version 2 : la frame et son en-tête de 64 octets, arrondis au Mio ; autant de slots
// que 64 Mio en contiennent, 32 au plus. slot_count < kMinSlots : la frame est trop grosse (4K BGRA).
struct Layout {
    uint32_t slot_size  = 0;
    uint32_t slot_count = 0;
    bool     ok() const { return slot_count >= kMinSlots; }
};

uint64_t    frame_bytes(uint32_t codec, uint32_t width, uint32_t height);  // 0 si pas un format décodé
Layout      raw_layout(uint32_t codec, uint32_t width, uint32_t height);
const char* codec_name(uint32_t codec);

// Matrice effective (jamais kColourAuto) : auto = BT.709 à partir de 720 lignes, BT.601 en dessous, plage limitée.
// Valeur inconnue (> 4) : traitée comme auto.
uint32_t    effective_colour(uint32_t colour, uint32_t height);
const char* colour_name(uint32_t effective);

// rgb = m[c][0..2] · (y, cb, cr) + m[c][3], avec y, cb, cr = octet / 255 (ce que lit le shader en UNORM).
struct YuvToRgb {
    float m[3][4];
};
YuvToRgb yuv_to_rgb(uint32_t effective);
struct Yuv8 {
    uint8_t y, cb, cr;
};
// Sens inverse (pour l'émetteur de test) : r, g, b dans [0, 1].
Yuv8 rgb_to_yuv(float r, float g, float b, uint32_t effective);
// Conversion CPU de référence d'un pixel (même calcul que le shader, arrondi à l'octet).
void yuv8_to_rgb8(const YuvToRgb& m, uint8_t y, uint8_t cb, uint8_t cr, uint8_t rgb[3]);

enum class HeaderStatus { NoStream, Unsupported, Ok };
// Vérifie un en-tête lu. `h` peut être corrigé (disposition de repli). why : raison si Unsupported ;
// note : avertissement non bloquant (disposition de repli, frame plus grande que le slot, couleur inconnue).
HeaderStatus check_header(Header& h, std::string* why, std::string* note);

struct SlotMeta {
    uint64_t seq         = 0;  // numéro de la frame dans la session
    uint64_t qpc_arrival = 0;
    uint32_t size        = 0;  // octets annoncés par l'émetteur
    uint64_t copied      = 0;  // octets réellement copiés (bornés par le slot et la taille d'une frame)
    int32_t  frames_lost = 0;
    uint32_t recovered   = 0;
    uint32_t flags       = 0;
    bool     truncated() const;  // bit 0, ou moins d'octets qu'une frame complète
};

// Lecteur : ne bloque jamais l'émetteur ; s'il a du retard, il saute à la frame la plus récente.
class Reader {
public:
    enum class State { Waiting, Streaming, Ended, Unsupported };
    enum class Read { None, Frame, Dropped };

    // base : vue de kTotalBytes octets (alignée sur 8), en lecture/écriture (reader_heartbeat).
    explicit Reader(uint8_t* base) : base_(base) {}

    // Relit l'en-tête (de façon cohérente : magic et session identiques avant et après) ; à appeler à chaque
    // poll. Une nouvelle session (ou un redémarrage de write_seq) remet le suivi à zéro.
    State refresh();

    // Copie la frame la plus récente pas encore lue : copy(const uint8_t* data, size_t bytes). La copie est
    // validée en relisant `seq` après coup ; si le slot a été réécrit pendant la copie, Read::Dropped.
    template <class F>
    Read read_latest(SlotMeta& meta, F&& copy);

    void heartbeat(uint64_t qpc) { a64(base_, off::reader_heartbeat).store(qpc, std::memory_order_release); }

    State              state() const { return state_; }
    const Header&      header() const { return hdr_; }  // valide en Streaming (et Unsupported pour le message)
    const std::string& reason() const { return why_; }  // Unsupported : pourquoi ; Streaming : avertissement
    uint64_t           frame_size() const { return frame_bytes_; }
    // Vrai une seule fois après le début d'une session (ou un redémarrage de write_seq).
    bool take_new_session() {
        const bool v = new_session_;
        new_session_ = false;
        return v;
    }
    uint64_t frames_read() const { return frames_read_; }
    uint64_t dropped() const { return dropped_; }    // slots réécrits pendant la copie
    uint64_t skipped() const { return skipped_; }    // frames jamais lues (lecteur en retard)
    uint64_t sessions() const { return sessions_; }  // sessions acceptées depuis la création

private:
    void     start_session(const Header& h);
    uint8_t* slot_ptr(uint64_t n) const {
        return base_ + kHeaderBytes + size_t(n % hdr_.slot_count) * size_t(hdr_.slot_size);
    }

    uint8_t*    base_;
    Header      hdr_;
    State       state_       = State::Waiting;
    bool        have_session_ = false, seen_stream_ = false, new_session_ = false, first_ = true;
    uint32_t    session_     = 0;
    uint64_t    next_seq_    = 0, frame_bytes_ = 0;
    uint64_t    frames_read_ = 0, dropped_ = 0, skipped_ = 0, sessions_ = 0;
    std::string why_;
};

template <class F>
Reader::Read Reader::read_latest(SlotMeta& meta, F&& copy) {
    if (state_ != State::Streaming) return Read::None;
    const uint64_t ws = a64(base_, off::write_seq).load(std::memory_order_acquire);
    if (ws < next_seq_) {  // l'émetteur est reparti de zéro sans changer de session : on repart aussi
        next_seq_    = 0;
        first_       = true;
        new_session_ = true;
    }
    if (ws == 0 || ws - 1 < next_seq_) return Read::None;  // rien de nouveau
    const uint64_t n = ws - 1;                               // la plus récente : on saute les autres
    if (!first_) skipped_ += n - next_seq_;
    first_    = false;
    next_seq_ = n + 1;

    uint8_t* slot = slot_ptr(n);
    if (a64(slot, off::slot::seq).load(std::memory_order_acquire) != n) {  // déjà repris par l'émetteur
        ++dropped_;
        return Read::Dropped;
    }
    meta             = SlotMeta{};
    meta.seq         = n;
    meta.qpc_arrival = a64(slot, off::slot::qpc_arrival).load(std::memory_order_relaxed);
    meta.size        = a32(slot, off::slot::size).load(std::memory_order_relaxed);
    meta.frames_lost = int32_t(a32(slot, off::slot::frames_lost).load(std::memory_order_relaxed));
    meta.recovered   = a32(slot, off::slot::recovered).load(std::memory_order_relaxed);
    meta.flags       = a32(slot, off::slot::flags).load(std::memory_order_relaxed);
    // Jamais au-delà du slot ni d'une frame complète, quoi que dise l'émetteur.
    meta.copied = std::min<uint64_t>({uint64_t(meta.size), uint64_t(hdr_.slot_size) - kSlotHeader, frame_bytes_});
    copy(static_cast<const uint8_t*>(slot + kSlotHeader), size_t(meta.copied));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (a64(slot, off::slot::seq).load(std::memory_order_acquire) != n) {  // réécrit pendant la copie
        ++dropped_;
        return Read::Dropped;
    }
    ++frames_read_;
    return Read::Frame;
}

}  // namespace tap
