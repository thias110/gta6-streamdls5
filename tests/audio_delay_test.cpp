// Tests du retard du son relayé (logique pure, sans Windows ni carte) : analyse de --audio-delay, retard
// cible, lissage, plan écrire/jeter du relais, file de trames, et simulation d'un relais complet.
// Compilation autonome : g++ -std=c++20 -Isrc tests/audio_delay_test.cpp src/capture/audio_delay.cpp
#include "capture/audio_delay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace audio_delay;

namespace {
int g_failures = 0;

void expect(bool ok, const char* what, double value) {
    std::printf("%s  %-66s %.6g\n", ok ? "OK   " : "ÉCHEC", what, value);
    if (!ok) ++g_failures;
}
}  // namespace

int main() {
    {  // --audio-delay
        Setting     s;
        std::string err;
        expect(parse(L"auto", s, &err) && s.mode == Mode::Auto, "auto", 0);
        expect(parse(L"AUTO", s, &err) && s.mode == Mode::Auto, "AUTO (casse ignorée)", 0);
        expect(parse(L"off", s, &err) && s.mode == Mode::Off, "off", 0);
        expect(parse(L"40", s, &err) && s.mode == Mode::Fixed && s.ms == 40.0f, "40", s.ms);
        expect(parse(L"37.5ms", s, &err) && s.mode == Mode::Fixed && s.ms == 37.5f, "37.5ms", s.ms);
        expect(parse(L"0", s, &err) && s.mode == Mode::Fixed && s.ms == 0.0f, "0", s.ms);
        int bad = 0;
        for (const wchar_t* v : {L"", L"-5", L"251", L"abc", L"40x", L"ms", L"nan"}) bad += parse(v, s, &err);
        expect(bad == 0 && !err.empty(), "valeurs invalides refusées (avec message)", bad);
        expect(describe({Mode::Fixed, 40.0f}) == "40 ms" && describe({}) == "auto" && describe({Mode::Off, 0}) == "off",
               "descriptions", 0);
    }
    {  // Retard cible
        expect(interpolation_extra_ms(false, 45.0f, 10.0f) == 0.0f, "auto sans FG : 0", 0);
        expect(interpolation_extra_ms(true, 45.0f, 10.0f) == 35.0f, "auto avec FG : retard - arrivée = 35 ms",
               interpolation_extra_ms(true, 45.0f, 10.0f));
        expect(interpolation_extra_ms(true, 5.0f, 10.0f) == 0.0f, "auto : jamais négatif", 0);
        expect(interpolation_extra_ms(true, 900.0f, 0.0f) == kMaxMs, "auto : borné à 250 ms", 0);
        expect(interpolation_extra_ms(true, NAN, 0.0f) == 0.0f, "auto : NaN -> 0", 0);
        expect(target_ms({Mode::Off, 0}, true, 45.0f, 10.0f) == 0.0f, "off : 0 même avec FG", 0);
        expect(target_ms({Mode::Fixed, 60.0f}, false, 45.0f, 10.0f) == 60.0f, "fixe : 60 ms même sans FG", 0);
        expect(target_ms({Mode::Auto, 0}, true, 45.0f, 10.0f) == 35.0f, "auto via target_ms", 0);
    }
    {  // Lissage
        Tracker t;
        expect(t.update(30.0f, 0.0) == 30.0f, "lissage : première valeur immédiate", t.current());
        expect(t.update(34.0f, 1.0) == 30.0f, "lissage : écart < 8 ms ignoré", t.current());
        expect(t.update(45.0f, 1.2) == 45.0f, "lissage : écart >= 8 ms appliqué", t.current());
        expect(t.update(60.0f, 1.4) == 45.0f, "lissage : au plus un changement par 0,5 s", t.current());
        expect(t.update(60.0f, 1.8) == 60.0f, "lissage : appliqué après 0,5 s", t.current());
        expect(t.update(0.0f, 1.85) == 0.0f, "lissage : retour à 0 immédiat (FG coupée)", t.current());
        expect(t.update(-5.0f, 3.0) == 0.0f, "lissage : négatif borné à 0", t.current());
    }
    {  // Conversions et plan
        expect(ms_to_frames(10.0f, 48000) == 480, "10 ms à 48 kHz = 480 trames", ms_to_frames(10.0f, 48000));
        expect(ms_to_frames(0.0f, 48000) == 0 && ms_to_frames(-1.0f, 48000) == 0, "0 / négatif = 0 trame", 0);
        Plan p = plan(480, 0, 2880, 0, 2880, 2880);
        expect(p.write == 480 && p.drop == 0, "retard 0 : tout part (relais d'origine)", p.write);
        p = plan(480, 1440, 2880, 0, 2880, 2880);
        expect(p.write == 0 && p.drop == 0, "retard 30 ms, 10 ms en file : on attend", p.write);
        p = plan(1920, 1440, 2880, 480, 2880, 2880);
        expect(p.write == 480 && p.drop == 0, "retard 30 ms, 40 ms en file : 10 ms partent", p.write);
        p = plan(1920, 1440, 100, 480, 2880, 2880);
        expect(p.write == 100, "écriture limitée à la place libre", p.write);
        p = plan(1440 + 2880 + 10, 1440, 2880, 0, 2880, 2880);
        expect(p.drop == 2890 && p.write == 0, "file > retard + 60 ms : on jette jusqu'au retard", p.drop);
        p = plan(1920, 1440, 2880, 3000, 2880, 2880);
        expect(p.write == 0 && p.drop == 480, "sortie > 60 ms : on jette ce qui serait parti", p.drop);
    }
    {  // File de trames (anneau qui boucle et grandit)
        FrameFifo f(4);
        uint8_t   in[4000], out[4000];
        for (int i = 0; i < 4000; ++i) in[i] = uint8_t(i * 7);
        f.push(in, 700);  // 2800 octets
        uint32_t got = f.pop(out, 300);
        bool     ok  = got == 300;
        for (int i = 0; i < 1200 && ok; ++i) ok = out[i] == in[i];
        f.push(in, 1000);  // boucle puis grandit
        f.push(nullptr, 2);  // silence
        ok &= f.frames() == 400 + 1000 + 2;
        got = f.pop(out, 400);
        for (int i = 0; i < 1600 && ok; ++i) ok = out[i] == in[1200 + i];
        got = f.pop(out, 1000);
        for (int i = 0; i < 4000 && ok; ++i) ok = out[i] == in[i];
        f.pop(out, 10);
        ok &= out[0] == 0 && out[7] == 0 && f.frames() == 0;
        expect(ok, "file : ordre, bouclage, croissance et silence", f.frames());
        f.push(in, 5);
        f.drop(3);
        expect(f.frames() == 2 && f.pop(out, 10) == 2 && out[0] == in[12], "file : drop puis pop partiel", 0);
    }
    {  // Simulation : capture 48 kHz par paquets de 10 ms, sortie consommée au même rythme ; le retard réel
       // (âge des trames quand elles sortent de la file) doit valoir le retard voulu.
        const uint32_t rate = 48000, pkt = 480, out_frames = 2880, max60 = 2880;
        FrameFifo      f(4);
        uint32_t       padding = 480, produced = 0, written_total = 0;
        double         worst = 0.0;
        for (int step = 0; step < 300; ++step) {
            const float    want   = step < 150 ? 0.0f : 35.0f;
            const uint32_t target = ms_to_frames(want, rate);
            std::vector<uint8_t> chunk(pkt * 4);
            for (uint32_t i = 0; i < pkt; ++i) std::memcpy(&chunk[i * 4], &produced, 4), ++produced;
            f.push(chunk.data(), pkt);
            padding = padding > pkt ? padding - pkt : 0;  // la sortie a joué 10 ms
            const Plan p = plan(f.frames(), target, out_frames - padding, padding, max60, max60);
            f.drop(p.drop);
            std::vector<uint8_t> o(size_t(p.write) * 4);
            const uint32_t       w = f.pop(o.data(), p.write);
            written_total += w;
            padding += w;
            if (w && step > 160) {  // âge (en trames) de la dernière trame écrite = retard de la file
                uint32_t idx = 0;
                std::memcpy(&idx, &o[(w - 1) * 4], 4);
                worst = std::max(worst, std::abs(double(produced - 1 - idx) - double(target)));
            }
        }
        expect(worst <= 1.0, "simulation : retard de la file = retard voulu (écart max en trames)", worst);
        expect(written_total > 0, "simulation : le son est bien relayé", written_total);
    }

    std::printf(g_failures ? "%d ÉCHEC(S)\n" : "TOUT OK\n", g_failures);
    return g_failures ? 1 : 0;
}
