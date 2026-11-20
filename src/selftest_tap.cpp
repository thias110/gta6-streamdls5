// Contrôle du frame tap sans PS5 ni GPU (WARP) : `rpenhance --selftest-tap [--tap-name NOM] [--warp]` lit les
// frames émises par tests/tap_writer, relit chaque texture BGRA convertie et vérifie contenu, drapeaux, sessions,
// arrêt et redémarrage de l'émetteur. Format : spécification MIT de SmoovPlay (docs/frame-tap.md, Copyright (c)
// 2026 the SmoovPlay authors ; THIRD-PARTY-NOTICES.md).
#include "selftest.hpp"

#include "capture/tap_pattern.hpp"
#include "capture/tap_source.hpp"
#include "pipeline/compositor.hpp"

#include <cmath>
#include <cstdarg>

namespace {

constexpr int      kSessions  = 2;     // tap_writer --sessions 2 (défaut)
constexpr double   kTimeout   = 90.0;  // secondes, émetteur compris
constexpr uint64_t kMinFrames = 10;    // frames vérifiées au moins par session
constexpr int      kMaxLogged = 30;    // erreurs détaillées au plus

struct Checker {
    int errors = 0;
    void fail(const char* fmt, ...) {
        ++errors;
        if (errors > kMaxLogged) return;
        char    buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log_msg("  ÉCHEC : %s", buf);
    }
};

struct SessionCheck {
    uint32_t session = 0;
    uint64_t frames = 0, fresh = 0, repeat = 0, lost = 0, recovered = 0, last_seq = 0;
    bool     any         = false;
    bool     ended_after = false;  // l'arrêt de l'émetteur a été vu après cette session
    int      worst       = 0;      // plus grand écart de couleur (0-255) sur les pastilles et le fond
};

struct Readback {
    ComPtr<ID3D11Texture2D> staging;
    uint32_t                w = 0, h = 0;
};

}  // namespace

int run_selftest_tap(const Config& cfg) {
    log_msg("== rpenhance selftest frame tap : %s ==", narrow(cfg.tap_name).c_str());
    D3DContext d3d;
    d3d.initialize(cfg.debug_layer, cfg.warp);
    Compositor comp;
    comp.init(d3d);
    TapFrameSource src(d3d, comp, cfg.tap_name);
    auto*          ctx = d3d.ctx();

    using State = tap::Reader::State;
    Checker                   ck;
    std::vector<SessionCheck> sessions;
    Readback                  rb;
    State                     prev    = src.state();
    int                       ends    = 0;
    uint64_t                  checked = 0;
    const double              t_end   = now_seconds() + kTimeout;

    while (now_seconds() < t_end) {
        WaitForSingleObject(src.wait_handle(), 50);  // event du tap, sinon toutes les 50 ms
        SourceFrame f;
        const bool  got = src.poll(f);
        const State st  = src.state();
        if (st != prev) {
            if (prev == State::Streaming && st == State::Ended) {
                ++ends;
                if (!sessions.empty()) sessions.back().ended_after = true;
            }
            prev = st;
        }
        if (st == State::Unsupported) {
            ck.fail("flux refusé : %s", src.reader().reason().c_str());
            break;
        }
        if (got) {
            const SourceFrameMeta& m = f.meta;
            if (!m.valid) ck.fail("métadonnées absentes");
            if (m.new_session) {
                if (!sessions.empty() && sessions.back().session == m.session)
                    ck.fail("redémarrage sans nouvelle session (session %u)", m.session);
                sessions.push_back(SessionCheck{});
                sessions.back().session = m.session;
                log_msg("Nouvelle session %u (première frame lue : n = %llu)", m.session,
                        static_cast<unsigned long long>(m.seq));
            } else if (sessions.empty()) {
                ck.fail("première frame sans début de session");
                sessions.push_back(SessionCheck{});
                sessions.back().session = m.session;
            }
            SessionCheck& s = sessions.back();
            if (m.session != s.session) ck.fail("session %u au lieu de %u", m.session, s.session);
            if (s.any && m.seq <= s.last_seq)
                ck.fail("numéros non croissants : %llu après %llu", static_cast<unsigned long long>(m.seq),
                        static_cast<unsigned long long>(s.last_seq));
            s.any      = true;
            s.last_seq = m.seq;
            ++s.frames;

            // Drapeaux et métadonnées attendus d'après le numéro (voir tests/tap_writer.cpp).
            const bool     even      = m.seq % 2 == 0;
            const int32_t  lost      = m.seq % 30 == 14 ? 2 : 0;
            const bool     recovered = m.seq % 30 == 16;
            if (m.fresh != even || m.repeat == even)
                ck.fail("n = %llu : drapeaux NEW=%d REPEAT=%d inattendus", static_cast<unsigned long long>(m.seq),
                        int(m.fresh), int(m.repeat));
            if (m.frames_lost != lost || m.recovered != recovered || m.truncated)
                ck.fail("n = %llu : frames_lost %d recovered %d tronquée %d inattendus",
                        static_cast<unsigned long long>(m.seq), m.frames_lost, int(m.recovered), int(m.truncated));
            s.fresh += m.fresh;
            s.repeat += m.repeat;
            s.lost += m.frames_lost > 0;
            s.recovered += m.recovered;
            const double now = now_seconds();
            if (!(f.time <= now + 0.01 && f.time > now - 1.0))
                ck.fail("n = %llu : horodatage %.4f s hors de [maintenant - 1 s, maintenant] (%.4f s)",
                        static_cast<unsigned long long>(m.seq), f.time, now);

            // Relecture de la texture BGRA convertie.
            const GpuTexture& t = *f.texture;
            if (t.format != DXGI_FORMAT_B8G8R8A8_UNORM || !t.srv || !t.rtv) ck.fail("texture de sortie non BGRA SRV+RT");
            if (!rb.staging || rb.w != t.width || rb.h != t.height) {
                D3D11_TEXTURE2D_DESC d{};
                t.tex->GetDesc(&d);
                d.Usage          = D3D11_USAGE_STAGING;
                d.BindFlags      = 0;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                d.MiscFlags      = 0;
                rb.staging.Reset();
                check_hr(d3d.device()->CreateTexture2D(&d, nullptr, &rb.staging), "staging selftest tap");
                rb.w = t.width;
                rb.h = t.height;
            }
            ctx->CopyResource(rb.staging.Get(), t.tex.Get());
            D3D11_MAPPED_SUBRESOURCE map{};
            check_hr(ctx->Map(rb.staging.Get(), 0, D3D11_MAP_READ, 0, &map), "Map selftest tap");
            const auto*    px    = static_cast<const uint8_t*>(map.pData);
            const uint32_t codec = src.reader().header().codec;
            const int      tol   = codec == tap::kNV12 ? 4 : 0;  // NV12 : quantification 8 bits aller-retour
            const uint32_t num   = tap_pattern::decode_frame(px, map.RowPitch);
            if (num != uint32_t(m.seq & 0xFFFFu))
                ck.fail("n = %llu : l'image porte le numéro %u", static_cast<unsigned long long>(m.seq), num);
            auto at   = [&](uint32_t x, uint32_t y) { return px + size_t(y) * map.RowPitch + size_t(x) * 4; };
            int  worst = 0;
            for (uint32_t k = 0; k < tap_pattern::kPatchCount; ++k) {
                const auto& e = tap_pattern::kPatches[k];
                for (int dy = -4; dy <= 4; ++dy)
                    for (int dx = -4; dx <= 4; ++dx) {
                        const uint8_t* p = at(uint32_t(int(tap_pattern::patch_cx(k)) + dx),
                                              uint32_t(int(tap_pattern::patch_cy()) + dy));
                        worst = std::max({worst, std::abs(p[2] - e.r), std::abs(p[1] - e.g), std::abs(p[0] - e.b)});
                    }
            }
            // Fond : R = numéro de l'image (n / 2), G = y / 4, B = x + image * 8 (pris loin du repli de B).
            const uint32_t picture = uint32_t(m.seq / 2);
            const uint32_t y       = t.height - 16;
            uint32_t       x       = t.width / 2;
            const int      v       = int((x + picture * 8) & 255u);
            x                      = uint32_t(int(x) + 128 - v);
            const tap_pattern::Rgb e = tap_pattern::pixel(x, y, uint32_t(m.seq), picture);
            const uint8_t*         p = at(x, y);
            const int bg = std::max({std::abs(p[2] - e.r), std::abs(p[1] - e.g), std::abs(p[0] - e.b)});
            if (worst > tol || bg > tol + 2)
                ck.fail("n = %llu : écart de couleur %d (pastilles), %d (fond, R = image %u) > tolérance %d",
                        static_cast<unsigned long long>(m.seq), worst, bg, picture & 255u, tol);
            s.worst = std::max({s.worst, worst, bg});
            ctx->Unmap(rb.staging.Get(), 0);
            if (checked++ < 4)
                log_msg("  frame n = %llu : numéro lu %u, NEW %d REPEAT %d, écart couleur %d / fond %d",
                        static_cast<unsigned long long>(m.seq), num, int(m.fresh), int(m.repeat), worst, bg);
        }
        if (sessions.size() >= size_t(kSessions) && sessions.back().ended_after) break;
    }

    // Bilan
    if (sessions.empty()) ck.fail("aucune frame reçue en %.0f s (tap_writer lancé avec le même --name ?)", kTimeout);
    if (sessions.size() < size_t(kSessions))
        ck.fail("%zu session(s) vue(s), %d attendue(s) (arrêt puis redémarrage de l'émetteur)", sessions.size(), kSessions);
    if (ends < kSessions) ck.fail("%d arrêt(s) de l'émetteur vu(s), %d attendu(s)", ends, kSessions);
    for (const auto& s : sessions) {
        log_msg("Session %u : %llu frames vérifiées (NEW %llu, REPEAT %llu, perte annoncée %llu, réparée %llu), "
                "dernière n = %llu, écart couleur max %d, arrêt vu : %s",
                s.session, static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.fresh),
                static_cast<unsigned long long>(s.repeat), static_cast<unsigned long long>(s.lost),
                static_cast<unsigned long long>(s.recovered), static_cast<unsigned long long>(s.last_seq), s.worst,
                s.ended_after ? "oui" : "non");
        if (s.frames < kMinFrames || s.fresh == 0 || s.repeat == 0)
            ck.fail("session %u : trop peu de frames (%llu, au moins %llu avec NEW et REPEAT)", s.session,
                    static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(kMinFrames));
    }
    const auto& r = src.reader();
    log_msg("Lecteur : %llu frames lues, %llu abandonnées (slot réécrit pendant la copie), %llu sautées (retard)",
            static_cast<unsigned long long>(r.frames_read()), static_cast<unsigned long long>(r.dropped()),
            static_cast<unsigned long long>(r.skipped()));
    if (ck.errors > kMaxLogged) log_msg("  (%d erreurs au total, %d détaillées)", ck.errors, kMaxLogged);
    log_msg(ck.errors == 0 ? "SELFTEST TAP OK" : "SELFTEST TAP ÉCHEC");
    return ck.errors == 0 ? 0 : 1;
}
