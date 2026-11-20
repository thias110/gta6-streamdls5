// Émetteur de test du frame tap (format SmoovPlay version 2, frames décodées), pour la CI de rpenhance.
//
// Copie adaptée de l'émetteur de référence de SmoovPlay, docs/frame-tap-writer.c
// (https://github.com/smoovplay/smoovplay), Copyright (c) 2026 the SmoovPlay authors, licence MIT (texte
// complet dans THIRD-PARTY-NOTICES.md). Modifications pour rpenhance (MIT) : C++, NV12 en plus du BGRA,
// image de test commune avec `rpenhance --selftest-tap` (src/capture/tap_pattern.hpp), plusieurs sessions
// (arrêt puis redémarrage du flux), attente du lecteur (reader_heartbeat), frames_lost/recovered de test.
//
//   tap_writer [--format nv12|bgra] [--width 1280] [--height 720] [--frames 120] [--sessions 2]
//              [--gap-ms 800] [--colour 0..4] [--name Local\rpenhance-tap-test] [--wait-reader 60] [--fps 60]
//
// Chaque session : `frames` frames à `fps` Hz ; la frame i montre l'image i / 2 (un jeu à 30 fps sur une
// sortie à 60 Hz) avec le drapeau NEW si i est pair, REPEAT sinon ; i % 30 == 14 annonce 2 frames perdues,
// i % 30 == 16 une image réparée (recovered). Code de sortie 0 si tout a été envoyé.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "capture/tap_pattern.hpp"
#include "capture/tap_protocol.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {

struct Tap {
    HANDLE   map = nullptr, event = nullptr;
    uint8_t* base = nullptr;
    uint32_t slot_count = 0, slot_size = 0;
};

volatile uint32_t* h32(Tap& t, size_t off) { return reinterpret_cast<volatile uint32_t*>(t.base + off); }
volatile uint64_t* h64(Tap& t, size_t off) { return reinterpret_cast<volatile uint64_t*>(t.base + off); }

// Ouvre (ou crée) le tap. L'un ou l'autre côté peut démarrer le premier.
bool tap_open(Tap& t, const std::wstring& name) {
    t.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(tap::kTotalBytes), name.c_str());
    if (!t.map) return false;
    t.base  = static_cast<uint8_t*>(MapViewOfFile(t.map, FILE_MAP_ALL_ACCESS, 0, 0, size_t(tap::kTotalBytes)));
    t.event = CreateEventW(nullptr, FALSE, FALSE, (name + L"-frame").c_str());
    return t.base != nullptr && t.event != nullptr;
}

// Début d'un flux de frames décodées : slots de Mio entiers (frame + en-tête de 64 octets), autant que 64 Mio
// en contiennent (32 au plus, 4 au moins).
bool tap_begin(Tap& t, uint32_t codec, uint32_t w, uint32_t h, uint32_t fps, uint32_t colour) {
    const tap::Layout l = tap::raw_layout(codec, w, h);
    if (!l.ok()) return false;
    t.slot_count = l.slot_count;
    t.slot_size  = l.slot_size;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(h32(t, tap::off::magic)), 0);  // en-tête en écriture
    *h32(t, tap::off::version)    = 2;  // version 2 : frames décodées
    *h32(t, tap::off::slot_count) = t.slot_count;
    *h32(t, tap::off::slot_size)  = t.slot_size;
    *h32(t, tap::off::codec)      = codec;
    *h32(t, tap::off::width)      = w;
    *h32(t, tap::off::height)     = h;
    *h32(t, tap::off::max_fps)    = fps;
    *h64(t, tap::off::qpc_freq)   = uint64_t(f.QuadPart);
    *h32(t, tap::off::features)   = 0;  // une frame par rafraîchissement, pas d'anneau son
    *h32(t, tap::off::colour)     = colour;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(h64(t, tap::off::write_seq)), 0);
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(h32(t, tap::off::session)));  // nouvelle session
    InterlockedExchange(reinterpret_cast<volatile LONG*>(h32(t, tap::off::magic)), LONG(tap::kMagic));  // en dernier
    return true;
}

// Une frame, lignes tassées. N'attend jamais le lecteur.
void tap_push(Tap& t, const uint8_t* data, uint32_t size, uint32_t flags, int32_t lost, uint32_t recovered) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint64_t seq  = *h64(t, tap::off::write_seq);
    uint8_t*       slot = t.base + tap::kHeaderBytes + size_t(seq % t.slot_count) * t.slot_size;
    if (size > t.slot_size - tap::kSlotHeader) {
        size = t.slot_size - tap::kSlotHeader;
        flags |= tap::kFlagTruncated;
    }
    *reinterpret_cast<volatile uint64_t*>(slot + 0) = tap::kBusy;  // en réécriture : un lecteur qui copie l'abandonne
    MemoryBarrier();
    *reinterpret_cast<volatile uint64_t*>(slot + 8)  = uint64_t(now.QuadPart);
    *reinterpret_cast<volatile uint32_t*>(slot + 16) = size;
    *reinterpret_cast<volatile int32_t*>(slot + 20)  = lost;
    *reinterpret_cast<volatile uint32_t*>(slot + 24) = recovered;
    *reinterpret_cast<volatile uint32_t*>(slot + 28) = flags;
    std::memcpy(slot + tap::kSlotHeader, data, size);
    MemoryBarrier();
    *reinterpret_cast<volatile uint64_t*>(slot + 0) = seq;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(h64(t, tap::off::write_seq)), LONG64(seq + 1));
    SetEvent(t.event);
}

void tap_end(Tap& t) {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(h32(t, tap::off::magic)), 0);
    SetEvent(t.event);
}

// Attend que le lecteur se manifeste (reader_heartbeat qui change), pour ne pas émettre dans le vide.
bool wait_reader(Tap& t, double seconds) {
    const uint64_t first = *h64(t, tap::off::reader_heartbeat);
    LARGE_INTEGER  f, t0, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {
        const uint64_t hb = *h64(t, tap::off::reader_heartbeat);
        if (hb != 0 && hb != first) return true;
        QueryPerformanceCounter(&now);
        if (double(now.QuadPart - t0.QuadPart) / double(f.QuadPart) > seconds) return false;
        Sleep(10);
    }
}

void sleep_until(LONGLONG due, LONGLONG freq) {
    LARGE_INTEGER now;
    for (QueryPerformanceCounter(&now); now.QuadPart < due; QueryPerformanceCounter(&now))
        Sleep(due - now.QuadPart > freq / 500 ? 1 : 0);  // plus de 2 ms : dormir, sinon céder
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    uint32_t     codec = tap::kNV12, w = 1280, h = 720, frames = 120, sessions = 2, gap_ms = 800, colour = 0, fps = 60;
    double       wait_s = 60.0;
    std::wstring name   = L"Local\\rpenhance-tap-test";
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() -> std::wstring {
            if (i + 1 >= argc) {
                std::fwprintf(stderr, L"valeur manquante après %ls\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        auto num = [&]() { return uint32_t(std::wcstoul(next().c_str(), nullptr, 10)); };
        if (a == L"--format") {
            const std::wstring v = next();
            if (v == L"nv12") codec = tap::kNV12;
            else if (v == L"bgra") codec = tap::kBGRA;
            else {
                std::fwprintf(stderr, L"--format attend nv12 ou bgra\n");
                return 2;
            }
        } else if (a == L"--width") w = num();
        else if (a == L"--height") h = num();
        else if (a == L"--frames") frames = num();
        else if (a == L"--sessions") sessions = num();
        else if (a == L"--gap-ms") gap_ms = num();
        else if (a == L"--colour") colour = num();
        else if (a == L"--fps") fps = std::max(1u, num());
        else if (a == L"--name") name = next();
        else if (a == L"--wait-reader") wait_s = std::wcstod(next().c_str(), nullptr);
        else {
            std::fwprintf(stderr, L"option inconnue : %ls\n", a.c_str());
            return 2;
        }
    }
    if (w < tap_pattern::kMinWidth || h < tap_pattern::kMinHeight || (codec == tap::kNV12 && ((w | h) & 1u))) {
        std::fprintf(stderr, "taille %ux%u invalide (au moins %ux%u, paire en NV12)\n", w, h, tap_pattern::kMinWidth,
                     tap_pattern::kMinHeight);
        return 2;
    }

    Tap t;
    if (!tap_open(t, name)) {
        std::fprintf(stderr, "impossible d'ouvrir le frame tap (erreur %lu)\n", static_cast<unsigned long>(GetLastError()));
        return 1;
    }
    if (wait_s > 0.0) {
        std::printf("attente du lecteur (reader_heartbeat), %.0f s au plus...\n", wait_s);
        std::fflush(stdout);
        if (!wait_reader(t, wait_s)) {
            std::fprintf(stderr, "aucun lecteur n'a écrit reader_heartbeat en %.0f s\n", wait_s);
            return 3;
        }
    }

    const uint32_t       eff  = tap::effective_colour(colour, h);
    const uint64_t       size = tap::frame_bytes(codec, w, h);
    std::vector<uint8_t> pic(size_t(size), 0);
    LARGE_INTEGER        f;
    QueryPerformanceFrequency(&f);
    for (uint32_t s = 0; s < sessions; ++s) {
        if (!tap_begin(t, codec, w, h, fps, colour)) {
            std::fprintf(stderr, "une frame %ux%u %s ne tient pas dans 64 Mio\n", w, h, tap::codec_name(codec));
            return 1;
        }
        std::printf("session %u : %s %ux%u, %u slots de %u octets, %u frames à %u Hz%s%s\n",
                    unsigned(*h32(t, tap::off::session)), tap::codec_name(codec), w, h, t.slot_count, t.slot_size,
                    frames, fps, codec == tap::kNV12 ? ", " : "", codec == tap::kNV12 ? tap::colour_name(eff) : "");
        std::fflush(stdout);
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        for (uint32_t i = 0; i < frames; ++i) {
            const uint32_t picture = i / 2;  // un jeu à 30 fps : une nouvelle image tous les deux rafraîchissements
            if (codec == tap::kNV12) tap_pattern::fill_nv12(pic.data(), w, h, i, picture, eff);
            else tap_pattern::fill_bgra(pic.data(), w, h, i, picture);
            const int32_t  lost      = i % 30 == 14 ? 2 : 0;
            const uint32_t recovered = i % 30 == 16 ? 1 : 0;
            tap_push(t, pic.data(), uint32_t(size), i % 2 == 0 ? tap::kFlagNew : tap::kFlagRepeat, lost, recovered);
            sleep_until(t0.QuadPart + LONGLONG(i + 1) * f.QuadPart / fps, f.QuadPart);
        }
        tap_end(t);
        std::printf("session %u terminée\n", unsigned(*h32(t, tap::off::session)));
        std::fflush(stdout);
        if (s + 1 < sessions) Sleep(gap_ms);  // émetteur arrêté : le lecteur doit le voir
    }
    std::printf("envoyé : %u session(s) de %u frames\n", sessions, frames);
    UnmapViewOfFile(t.base);
    CloseHandle(t.map);
    CloseHandle(t.event);
    return 0;
}
