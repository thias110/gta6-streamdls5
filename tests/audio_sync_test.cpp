// Tests du relais audio entre deux horloges (logique pure, sans Windows ni carte) : rééchantillonneur
// linéaire, asservissement de la dérive, simulation d'un relais complet carte -> sortie avec des quartz qui
// dérivent (±300 ppm), des changements du retard voulu et une coupure de l'entrée ; formats (décodage,
// downmix, encodage, formats de sortie essayés).
// Compilation autonome : g++ -std=c++20 -Isrc tests/audio_sync_test.cpp src/capture/audio_sync.cpp
//                        src/capture/audio_delay.cpp src/capture/audio_format.cpp
#include "capture/audio_format.hpp"
#include "capture/audio_sync.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <vector>

using namespace audio_sync;

namespace {
int g_failures = 0;

void expect(bool ok, const char* what, double value) {
    std::printf("%s  %-70s %.6g\n", ok ? "OK   " : "ÉCHEC", what, value);
    if (!ok) ++g_failures;
}

struct SimResult {
    uint64_t underruns = 0, dropped = 0, reprimes = 0, written_over_room = 0;
    double   final_ppm = 0.0;  // correction moyenne en fin de simulation
    // Âge moyen (ms) du son joué, par segment de retard voulu (fin de chaque segment, une fois stabilisé).
    std::vector<double> seg_age_ms;
    double              max_age_dev_ms = 0.0;  // écart max à la moyenne du segment (gigue de latence)
};

// Simulation : capture 480 trames par paquet au rythme de l'horloge de la carte (48 kHz * (1 + drift)),
// sortie qui joue 480 trames toutes les 10 ms exactes (horloge de la sortie), tampon de sortie de 60 ms.
// Les trames portent leur numéro (+1, 0 = silence) : l'âge d'une trame jouée = latence réelle du relais.
SimResult simulate(double drift_ppm, const std::vector<std::pair<double, float>>& delays, double duration,
                   double stall_at = -1.0, double stall_len = 0.0) {
    const double   in_rate = 48000.0 * (1.0 + drift_ppm * 1e-6);
    const uint32_t pkt = 480, out_period = 480, out_buffer = 2880;
    PumpConfig     cfg;
    cfg.channels = 1;
    Pump              pump(cfg);
    std::deque<float> outq(size_t(audio_delay::ms_to_frames(cfg.out_fill_ms, 48000)), 0.0f);  // amorçage
    std::vector<float> out(out_buffer);
    SimResult          r;
    double             next_cap = 0.0013, next_out = 0.0100, ppm_sum = 0.0, ppm_n = 0.0;
    uint64_t           stalled_frames = 0;  // trames jamais capturées (coupure) : décalent les numéros
    uint64_t           produced = 0;
    size_t             seg      = 0;
    std::vector<std::vector<double>> ages(delays.size());
    auto delay_at = [&](double t) {
        float d = 0.0f;
        for (auto& [t0, ms] : delays)
            if (t >= t0) d = ms;
        return d;
    };
    auto seg_at = [&](double t) {
        size_t s = 0;
        for (size_t i = 0; i < delays.size(); ++i)
            if (t >= delays[i].first) s = i;
        return s;
    };
    while (std::min(next_cap, next_out) < duration) {
        if (next_cap <= next_out) {
            const double t       = next_cap;
            const bool   stalled = stall_at >= 0.0 && t >= stall_at && t < stall_at + stall_len;
            if (stalled) stalled_frames += pkt;
            if (!stalled) {
                std::vector<float> p(pkt);
                for (auto& v : p) v = float(++produced);
                pump.push(p.data(), pkt);
                const uint32_t room = out_buffer - uint32_t(outq.size());
                const uint32_t w    = pump.pull(out.data(), room, uint32_t(outq.size()), delay_at(t), t);
                if (w > room) ++r.written_over_room;
                outq.insert(outq.end(), out.begin(), out.begin() + w);
            }
            next_cap += double(pkt) / in_rate;
        } else {
            const double t = next_out;
            if (t > 5.0 && outq.size() < out_period && !(stall_at >= 0.0 && t >= stall_at && t < stall_at + 1.0))
                ++r.underruns;
            for (uint32_t i = 0; i < out_period && !outq.empty(); ++i) {
                const float v = outq.front();
                outq.pop_front();
                if (v > 0.0f && i == 0) {
                    // Instant de capture de la trame v (fin de son paquet) : horloge continue de la carte.
                    const double cap = 0.0013 + (double(v) - 1.0 + double(stalled_frames)) / in_rate;
                    const double age = (t - cap) * 1000.0;
                    seg              = seg_at(t);
                    const double t0  = delays[seg].first;
                    const double t1  = seg + 1 < delays.size() ? delays[seg + 1].first : duration;
                    if (t > t0 + (t1 - t0) * 0.6) ages[seg].push_back(age);  // fin du segment : régime établi
                }
            }
            if (t > duration - 250.0) {
                ppm_sum += pump.ppm();
                ppm_n += 1.0;
            }
            next_out += 0.010;
        }
    }
    for (auto& a : ages) {
        double m = 0.0;
        for (double x : a) m += x;
        m = a.empty() ? -1.0 : m / double(a.size());
        r.seg_age_ms.push_back(m);
        for (double x : a) r.max_age_dev_ms = std::max(r.max_age_dev_ms, std::abs(x - m));
    }
    r.dropped   = pump.dropped();
    r.reprimes  = pump.reprimes();
    if (std::getenv("AUDIO_SYNC_DEBUG"))
        std::printf("  [sim] relayées %llu, silence %llu, jetées %llu, réamorçages %llu\n",
                    (unsigned long long)pump.relayed(), (unsigned long long)pump.silence(),
                    (unsigned long long)pump.dropped(), (unsigned long long)pump.reprimes());
    r.final_ppm = ppm_n > 0.0 ? ppm_sum / ppm_n : 0.0;  // moyenne des 250 dernières secondes
    return r;
}
}  // namespace

int main() {
    {  // Rééchantillonneur : pas 1 = recopie (retardée d'une trame)
        LinearResampler   rs(2);
        std::vector<float> in(20), out(20);
        for (int i = 0; i < 20; ++i) in[size_t(i)] = float(i + 1);
        const uint32_t need = rs.input_needed(10, 1.0);
        const uint32_t used = rs.process(in.data(), need, out.data(), 10, 1.0);
        bool ok = need == 10 && used == 10 && out[0] == 0.0f && out[1] == 0.0f;
        for (int k = 1; k < 10 && ok; ++k) ok = out[size_t(2 * k)] == in[size_t(2 * (k - 1))];
        expect(ok, "rééchantillonneur : pas 1 = recopie, une trame de retard", need);
    }
    {  // Pas 0,5 (sur-échantillonnage x2) d'une rampe : interpolation exacte
        LinearResampler   rs(1);
        std::vector<float> in(100), out(150);
        for (int i = 0; i < 100; ++i) in[size_t(i)] = float(i);
        const uint32_t n    = rs.output_possible(50, 0.5);
        const uint32_t used = rs.process(in.data(), 50, out.data(), n, 0.5);
        double         err  = 0.0;
        for (uint32_t k = 2; k < n; ++k) err = std::max(err, std::abs(double(out[k]) - (double(k) * 0.5 - 1.0)));
        expect(n >= 98 && used <= 50 && err < 1e-5, "rééchantillonneur : rampe au pas 0,5, interpolation exacte", err);
    }
    {  // Découpage en paquets = traitement d'un bloc ; jamais de lecture au-delà de ce qui est demandé
        const double       step = 1.0003;
        std::vector<float> in(48000);
        for (size_t i = 0; i < in.size(); ++i) in[i] = float(std::sin(double(i) * 0.01) * 0.5);
        LinearResampler    a(1), b(1);
        std::vector<float> one(47000), two;
        const uint32_t     n_one = 47000;
        a.process(in.data(), a.input_needed(n_one, step), one.data(), n_one, step);
        size_t   pos  = 0;
        uint32_t made = 0;
        bool     ok   = true;
        while (made < n_one) {
            const uint32_t chunk = std::min<uint32_t>(n_one - made, 37 + (made % 400));
            const uint32_t need  = b.input_needed(chunk, step);
            ok &= pos + need <= in.size();
            std::vector<float> o(chunk);
            pos += b.process(in.data() + pos, need, o.data(), chunk, step);
            two.insert(two.end(), o.begin(), o.end());
            made += chunk;
        }
        double diff = 0.0;
        for (uint32_t i = 0; i < n_one; ++i) diff = std::max(diff, double(std::abs(one[i] - two[i])));
        expect(ok && diff < 1e-5, "rééchantillonneur : paquets irréguliers = un seul bloc", diff);
        expect(std::abs(double(pos) - n_one * step) <= 1.0, "rééchantillonneur : trames consommées = sorties x pas",
               double(pos) - n_one * step);
    }
    {  // output_possible / input_needed cohérents
        LinearResampler rs(1);
        bool            ok = true;
        for (double step : {0.9187, 0.9995, 1.0, 1.0004, 1.0884})
            for (uint32_t n_in : {1u, 2u, 7u, 480u, 481u, 1000u}) {
                const uint32_t n = rs.output_possible(n_in, step);
                ok &= rs.input_needed(n, step) <= n_in && rs.input_needed(n + 1, step) > n_in;
            }
        expect(ok, "rééchantillonneur : output_possible maximal et sans débordement", 0);
    }
    {  // Asservissement : signe, borne, intégrale
        DriftController c;
        expect(c.update(0.0, 0.01) == 0.0, "dérive : erreur nulle -> 0 ppm", c.ppm());
        c.reset();
        c.update(2.0, 0.0);
        expect(c.ppm() > 0.0, "dérive : trop de son en attente -> on consomme plus vite", c.ppm());
        c.reset();
        c.update(-2.0, 0.0);
        expect(c.ppm() < 0.0, "dérive : pas assez -> on consomme moins vite", c.ppm());
        c.reset();
        for (int i = 0; i < 1000; ++i) c.update(100.0, 0.01);
        expect(c.ppm() == DriftController::kMaxPpm, "dérive : correction bornée à kMaxPpm", c.ppm());
        c.reset();
        for (int i = 0; i < 10000; ++i) c.update(std::nan(""), 0.01);
        expect(c.ppm() == 0.0, "dérive : mesure NaN ignorée", c.ppm());
    }

    // --- Simulations ---------------------------------------------------------------------------------------
    const std::vector<std::pair<double, float>> steady = {{0.0, 0.0f}};
    for (double drift : {0.0, 300.0, -300.0, 80.0, -15.0}) {
        const SimResult r = simulate(drift, steady, 600.0);
        char            what[160];
        std::snprintf(what, sizeof(what), "dérive %+.0f ppm : rien de jeté, aucune famine", drift);
        expect(r.dropped == 0 && r.underruns == 0 && r.reprimes == 0 && r.written_over_room == 0, what,
               double(r.dropped + r.underruns + r.reprimes));
        std::snprintf(what, sizeof(what), "dérive %+.0f ppm : latence moyenne 15-40 ms (retard 0)", drift);
        expect(r.seg_age_ms[0] > 15.0 && r.seg_age_ms[0] < 40.0, what, r.seg_age_ms[0]);
        std::snprintf(what, sizeof(what), "dérive %+.0f ppm : latence stable (écart max, ms)", drift);
        expect(r.max_age_dev_ms < 8.0, what, r.max_age_dev_ms);
        // Sous ~50 ppm, le battement padding/paquets (période 10 ms / dérive) dépasse la durée simulée.
        if (std::abs(drift) >= 50.0) {
            std::snprintf(what, sizeof(what), "dérive %+.0f ppm : correction moyenne = dérive (ppm)", drift);
            expect(std::abs(r.final_ppm - drift) < 25.0, what, r.final_ppm);
        }
    }
    {  // Retard voulu qui change (interpolation, --audio-delay) : la latence suit, sans famine
        const SimResult r = simulate(200.0, {{0.0, 0.0f}, {80.0, 35.0f}, {160.0, 10.0f}, {240.0, 0.0f}}, 320.0);
        const double    base = r.seg_age_ms[0];
        double          worst = 0.0;
        const float     want[] = {0.0f, 35.0f, 10.0f, 0.0f};
        for (size_t i = 0; i < 4; ++i) worst = std::max(worst, std::abs(r.seg_age_ms[i] - base - want[i]));
        expect(worst < 2.0, "retard 0 -> 35 -> 10 -> 0 ms : latence = base + retard (écart max, ms)", worst);
        expect(r.underruns == 0 && r.reprimes == 0, "retard variable : aucune famine", double(r.underruns));
    }
    {  // Entrée coupée 300 ms (signal HDMI perdu) : la file garde son contenu, rien n'est jeté, latence normale
        const SimResult r = simulate(-150.0, {{0.0, 20.0f}}, 120.0, 30.0, 0.3);
        expect(r.dropped == 0, "entrée coupée : rien de jeté", double(r.dropped));
        expect(r.seg_age_ms[0] > 30.0 && r.seg_age_ms[0] < 55.0, "entrée coupée : latence rétablie (retard 20 ms)",
               r.seg_age_ms[0]);
        expect(r.underruns == 0, "entrée coupée : pas de famine hors coupure", double(r.underruns));
    }
    {  // Amorçage puis gros déficit (l'entrée n'a pas suivi) : silence ajouté d'un coup, puis le son reprend
        PumpConfig cfg;
        cfg.channels = 1;
        Pump               p(cfg);
        std::vector<float> in(2000, 0.25f), out(2880);
        const uint32_t     w0 = p.pull(out.data(), 2880, 0, 0.0f, 0.00);  // file vide : amorçage (silence)
        expect(!p.started() && w0 == 960 && out[0] == 0.0f && p.silence() == 960, "amorçage : silence", double(w0));
        p.push(in.data(), 2000);
        p.pull(out.data(), 2880, 0, 0.0f, 0.01);
        p.pull(out.data(), 2880, 0, 0.0f, 0.02);  // file ~80 trames, sortie vide : déficit ~ 23 ms
        const uint32_t w3 = p.pull(out.data(), 2880, 0, 0.0f, 0.03);
        expect(p.started() && p.reprimes() == 1 && w3 == 960 && out[0] == 0.0f && out[959] == 0.0f,
               "déficit > 20 ms : silence d'un coup", double(p.reprimes()));
        p.push(in.data(), 1440);
        const uint64_t sil = p.silence();
        const uint32_t w4  = p.pull(out.data(), 2880, 0, 0.0f, 0.04);
        const uint32_t s4  = uint32_t(p.silence() - sil);
        expect(w4 == 960 && s4 > 0 && s4 < 960 && out[s4 - 1] == 0.0f && out[w4 - 1] > 0.2f,
               "déficit : reste du silence, puis le son reprend", double(s4));
    }
    // --- Formats -------------------------------------------------------------------------------------------
    {
        namespace af = audio_format;
        const auto id = af::mix_matrix(2, 0, 2);
        expect(af::is_identity(id, 2, 2), "format : stéréo -> stéréo = identité", 0);
        const auto up = af::mix_matrix(1, 0, 2);
        expect(up[0] == 1.0f && up[1] == 1.0f, "format : mono -> stéréo recopié", 0);
        // 5.1 (FL FR FC LFE BL BR) -> stéréo
        const auto dm = af::mix_matrix(6, 0, 2);
        float      sumL = 0.0f, sumR = 0.0f;
        for (int c = 0; c < 6; ++c) sumL += dm[size_t(c)], sumR += dm[size_t(6 + c)];
        expect(dm[3] == 0.0f && dm[9] == 0.0f, "format : 5.1 -> stéréo, LFE ignoré", 0);
        expect(dm[1] == 0.0f && dm[6] == 0.0f && dm[0] > dm[2] && dm[2] == dm[8], "format : 5.1 -> stéréo, G/D séparés, centre partagé", dm[2]);
        expect(std::abs(sumL - 1.0f) < 1e-5f && std::abs(sumR - 1.0f) < 1e-5f, "format : 5.1 -> stéréo normalisé (pas d'écrêtage)", sumL);
        // 7.1 masque explicite -> stéréo
        const uint32_t m71 = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x200 | 0x400;
        const auto     d7  = af::mix_matrix(8, m71, 2);
        expect(d7[6] > 0.0f && d7[8 + 6] == 0.0f && d7[8 + 7] > 0.0f, "format : 7.1 -> stéréo, latéraux du bon côté", d7[6]);
        const auto mono = af::mix_matrix(6, 0, 1);
        expect(std::abs(mono[0] - 0.2f) < 1e-6f && mono[3] == 0.0f, "format : 5.1 -> mono = moyenne hors LFE", mono[0]);

        // Décodage int16 5.1 -> stéréo float
        int16_t            pcm[12] = {16384, 0, 0, 32767, 0, 0, /**/ 0, -16384, 0, 0, 0, 0};
        float              outf[4];
        std::vector<float> scratch;
        af::Pcm            in{48000, 6, af::Sample::Int16, 0};
        af::to_float(reinterpret_cast<const uint8_t*>(pcm), 2, in, 2, dm, outf, scratch);
        expect(std::abs(outf[0] - 0.5f * dm[0]) < 1e-5f && outf[1] == 0.0f && std::abs(outf[3] + 0.5f * dm[7]) < 1e-5f,
               "format : décodage int16 + downmix", outf[0]);
        // int24 et int32 -> float -> int16 / int24 / float (aller-retour)
        uint8_t i24[6] = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0};  // +0.5, -0.5
        af::to_float(i24, 1, {48000, 2, af::Sample::Int24, 0}, 2, af::mix_matrix(2, 0, 2), outf, scratch);
        expect(outf[0] == 0.5f && outf[1] == -0.5f, "format : décodage int24", outf[0]);
        int32_t i32[2] = {int32_t(0x40000000), int32_t(0xC0000000)};
        af::to_float(reinterpret_cast<const uint8_t*>(i32), 1, {48000, 2, af::Sample::Int32, 0}, 2,
                     af::mix_matrix(2, 0, 2), outf, scratch);
        expect(outf[0] == 0.5f && outf[1] == -0.5f, "format : décodage int32", outf[0]);
        const float src[4] = {1.5f, -2.0f, 0.5f, -0.25f};
        int16_t     o16[4];
        af::from_float(src, 2, {48000, 2, af::Sample::Int16, 0}, reinterpret_cast<uint8_t*>(o16));
        expect(o16[0] == 32767 && o16[1] == -32767 && o16[2] == 16384 && o16[3] == -8192, "format : encodage int16 écrêté", o16[0]);
        uint8_t o24[12];
        af::from_float(src, 2, {48000, 2, af::Sample::Int24, 0}, o24);
        float back[4];
        af::to_float(o24, 2, {48000, 2, af::Sample::Int24, 0}, 2, af::mix_matrix(2, 0, 2), back, scratch);
        expect(std::abs(back[2] - 0.5f) < 1e-6f && std::abs(back[0] - 1.0f) < 1e-6f, "format : aller-retour int24", back[2]);

        // Formats de sortie essayés
        const af::Pcm f2{48000, 2, af::Sample::Float32, 0x3}, f6{48000, 6, af::Sample::Int16, 0x3F},
            mix2{44100, 2, af::Sample::Float32, 0x3}, mix6{48000, 6, af::Sample::Float32, 0x3F};
        auto c = af::output_candidates(f2, mix2);
        expect(c.size() == 3 && c[0].fmt.sample == af::Sample::Float32 && c[0].fmt.channels == 2 && c[0].autoconvert &&
                   c[1].fmt.sample == af::Sample::Int16 && !c[2].autoconvert && c[2].fmt.rate == 44100,
               "sortie : stéréo float, int16, puis format de mixage exact", double(c.size()));
        c = af::output_candidates(f6, mix2);
        bool stereo_only = !c.empty();
        for (const auto& x : c) stereo_only &= x.fmt.channels == 2;
        expect(stereo_only && c[0].fmt.sample == af::Sample::Float32, "sortie : 5.1 vers sortie stéréo -> downmix stéréo",
               double(c.size()));
        c = af::output_candidates(f6, mix6);
        expect(c[0].fmt.channels == 6 && c[0].fmt.mask == 0x3F && c[2].fmt.channels == 2,
               "sortie : 5.1 vers sortie 5.1 -> 5.1 d'abord, stéréo en repli", double(c.size()));
        c = af::output_candidates(f2, af::Pcm{});
        expect(c.size() == 2, "sortie : format de mixage inconnu -> pas de candidat exact", double(c.size()));
    }
    {  // Rééchantillonnage 44,1 kHz -> 48 kHz (format de mixage exact) : la file reste stable
        PumpConfig cfg;
        cfg.channels = 1;
        cfg.in_rate  = 44100;
        Pump               p(cfg);
        std::vector<float> in(441, 0.5f), out(2880);
        uint32_t           padding = 960;
        uint64_t           total   = 0;
        for (int i = 0; i < 3000; ++i) {  // 30 s : paquets de 10 ms des deux côtés
            p.push(in.data(), 441);
            padding = padding > 480 ? padding - 480 : 0;
            const uint32_t w = p.pull(out.data(), 2880 - padding, padding, 0.0f, i * 0.01);
            padding += w;
            total += w;
        }
        expect(p.dropped() == 0 && p.reprimes() == 0 && std::abs(double(total) - 3000.0 * 480) < 2000.0,
               "44,1 -> 48 kHz : débit de sortie = 48 kHz, rien de jeté", double(total));
    }

    {  // Réouverture après erreur du périphérique : temporisation croissante, bornée
        expect(retry_delay_s(0) == 0.5 && retry_delay_s(1) == 1.0 && retry_delay_s(2) == 2.0 && retry_delay_s(3) == 4.0 &&
                   retry_delay_s(4) == 5.0 && retry_delay_s(100) == 5.0 && retry_delay_s(-3) == 0.5,
               "réouverture : 0,5 / 1 / 2 / 4 / 5 s", retry_delay_s(4));
        double total = 0.0;
        for (int i = 0; i < kMaxRetries; ++i) total += retry_delay_s(i);
        expect(total > 60.0 && total < 180.0, "réouverture : abandon après 1 à 3 minutes", total);
    }

    std::printf(g_failures ? "%d ÉCHEC(S)\n" : "TOUT OK\n", g_failures);
    return g_failures ? 1 : 0;
}
