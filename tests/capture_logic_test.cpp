// Tests de la logique pure de la source carte de capture (sans Windows, sans GPU, sans carte) :
// horodatage MF -> horloge now_seconds(), choix du mode natif, ordre des formats de sortie, choix du
// périphérique, et classement des doublons sur un signal « câble » (jeu 30 fps dans un signal 60 Hz).
// Compilation autonome : g++ -std=c++20 -Isrc tests/capture_logic_test.cpp src/capture/capture_logic.cpp
//                        src/pipeline/dedupe.cpp
#include "capture/capture_logic.hpp"
#include "pipeline/dedupe.hpp"

#include <cmath>
#include <cstdio>
#include <random>
#include <string>

using namespace capture_logic;

namespace {
int g_failures = 0;

void expect(bool ok, const char* what, double value) {
    std::printf("%s  %-66s %.6g\n", ok ? "OK   " : "ÉCHEC", what, value);
    if (!ok) ++g_failures;
}

int64_t hns(double seconds) { return int64_t(std::llround(seconds * 1e7)); }
}  // namespace

int main() {
    // --- Conversion 100 ns -> secondes -----------------------------------------------------------------
    expect(hns_to_seconds(10'000'000) == 1.0, "100 ns : 10 000 000 = 1 s", hns_to_seconds(10'000'000));
    expect(std::abs(hns_to_seconds(166'667) - 0.0166667) < 1e-9, "100 ns : 166 667 = 16,6667 ms",
           hns_to_seconds(166'667));
    expect(hns_to_seconds(-5'000'000) == -0.5, "100 ns : valeurs négatives", hns_to_seconds(-5'000'000));
    // Grand horodatage QPC (plusieurs jours de fonctionnement) : précision sous la microseconde.
    expect(std::abs(hns_to_seconds(hns(400000.123456)) - 400000.123456) < 1e-6, "100 ns : 400 000 s, précision < 1 µs",
           hns_to_seconds(hns(400000.123456)));

    {  // 1. Horloge QPC du pilote plausible : utilisée telle quelle (même base que now_seconds()).
        TimestampMapper m;
        const double t = m.map(hns(1000.000), hns(5.0), true, 1000.012);
        expect(std::abs(t - 1000.000) < 1e-9 && m.basis() == TimestampMapper::Basis::Device,
               "pilote : horodatage QPC repris (retard 12 ms)", t);
        const double f = m.map(hns(1000.0335), hns(5.0167), false, 1000.0325);  // 1 ms dans le futur : toléré
        expect(f <= 1000.0325 && m.basis() == TimestampMapper::Basis::Device, "pilote : légère avance bornée à l'arrivée",
               f);
    }
    {  // 2. Horloge du pilote aberrante (autre base) : repli sur l'horodatage MF recalé.
        TimestampMapper m;
        const double t = m.map(hns(3.0), hns(7.0), true, 1000.0);
        expect(m.basis() == TimestampMapper::Basis::Mapped && std::abs(t - 1000.0) < 1e-9,
               "pilote aberrant : horodatage MF recalé sur l'arrivée", t);
    }
    {  // 3. Horodatage MF + gigue de livraison : le décalage suit la borne basse, la gigue disparaît.
        TimestampMapper m;
        std::mt19937    rng(7);
        std::uniform_real_distribution<double> jitter(0.0, 0.006);  // 0 à 6 ms de retard de livraison
        const double base = 5000.0, period = 1.0 / 60.0, lat = 0.020;
        double max_err = 0.0, max_step_err = 0.0, prev = 0.0;
        bool   never_after_arrival = true;
        for (int i = 0; i < 600; ++i) {
            const double cap     = base + i * period;
            const double arrival = cap + lat + jitter(rng);
            const double t       = m.map(std::nullopt, hns(12.5 + i * period), i == 0, arrival);
            never_after_arrival &= t <= arrival + 1e-12;
            if (i >= 120) {  // après stabilisation : intervalles réguliers (gigue brute : jusqu'à 6 ms)
                max_step_err = std::max(max_step_err, std::abs((t - prev) - period));
                max_err      = std::max(max_err, std::abs((t - cap) - lat));
            }
            prev = t;
        }
        expect(never_after_arrival, "MF recalé : jamais postérieur à l'arrivée", never_after_arrival);
        expect(max_step_err < 3e-4, "MF recalé : intervalles à 0,3 ms près malgré 6 ms de gigue (écart max s)",
               max_step_err);
        expect(max_err < 0.0011, "MF recalé : à moins de ~1 ms de (capture + latence min)", max_err);
    }
    {  // 4. Dérive d'horloge (horloge de la carte 100 ppm plus lente que QPC : le décalage monte) : suivie.
        TimestampMapper m;
        double worst = 0.0;
        for (int i = 0; i < 60 * 120; ++i) {  // 2 minutes
            const double cap     = 100.0 + i / 60.0;
            const double arrival = cap + 0.010;
            const double card    = (i / 60.0) * (1.0 - 100e-6);  // horloge de la carte
            const double t       = m.map(std::nullopt, hns(card), i == 0, arrival);
            if (i > 600) worst = std::max(worst, arrival - t);
        }
        expect(worst < 0.002, "dérive 100 ppm sur 2 min : retard apparent reste < 2 ms (s)", worst);
    }
    {  // 5. Discontinuité / retour en arrière / trou > 1 s : recalage immédiat.
        TimestampMapper m;
        m.map(std::nullopt, hns(10.0), true, 50.010);
        m.map(std::nullopt, hns(10.0167), false, 50.0267);
        const double back = m.map(std::nullopt, hns(2.0), false, 50.050);  // flux redémarré
        expect(std::abs(back - 50.050) < 1e-9, "horodatage qui recule : recalage sur l'arrivée", back);
        const double gap = m.map(std::nullopt, hns(9.0), false, 57.060);  // trou de 7 s
        expect(std::abs(gap - 57.060) < 1e-9, "trou > 1 s : recalage sur l'arrivée", gap);
        m.map(std::nullopt, hns(9.0167), false, 57.075);
        const double disc = m.map(std::nullopt, hns(9.0334), true, 57.100);
        expect(std::abs(disc - 57.100) < 1e-9, "discontinuité signalée : recalage", disc);
    }
    {  // 6. Aucun horodatage : l'arrivée.
        TimestampMapper m;
        const double t = m.map(std::nullopt, std::nullopt, false, 42.0);
        expect(t == 42.0 && m.basis() == TimestampMapper::Basis::Arrival, "sans horodatage : arrivée", t);
    }
    {  // 7. Horodatage figé (pilote qui renvoie toujours 0) : arrivée, sans recalage permanent.
        TimestampMapper m;
        const double a = m.map(std::nullopt, 0, true, 10.000);
        const double b = m.map(std::nullopt, 0, false, 10.017);
        const double c = m.map(std::nullopt, 0, false, 10.033);
        expect(a == 10.000 && b == 10.017 && c == 10.033 && m.basis() == TimestampMapper::Basis::Arrival,
               "horodatage toujours 0 : arrivée", c);
        const double d = m.map(std::nullopt, hns(0.0167), false, 10.051);  // le pilote se met à horodater
        expect(m.basis() == TimestampMapper::Basis::Mapped && d <= 10.051 && d > c, "puis horodatage valide : recalé", d);
        const double neg = m.map(std::nullopt, -5, false, 10.070);
        expect(neg == 10.070 && m.basis() == TimestampMapper::Basis::Arrival, "horodatage négatif : arrivée", neg);
    }
    {  // 8. Horloge du pilote qui recule de 5 ms (à-coup) : le résultat ne recule jamais, ni ne se répète.
        TimestampMapper m;
        const double a = m.map(hns(100.000), std::nullopt, true, 100.010);
        const double b = m.map(hns(100.0167), std::nullopt, false, 100.027);
        const double c = m.map(hns(100.0117), std::nullopt, false, 100.044);  // recule de 5 ms
        const double d = m.map(hns(100.0117), std::nullopt, false, 100.050);  // répété
        expect(a < b && b < c && c < d && c <= 100.044 && d <= 100.050, "pilote qui recule : résultat croissant",
               c - b);
        const double e = m.map(hns(99.0), std::nullopt, true, 99.010);  // discontinuité : recalage permis
        expect(e == 99.0, "discontinuité : retour en arrière accepté", e);
    }
    {  // 9. Horloge du pilote hors bornes (0, futur, trop vieux) : jamais utilisée.
        TimestampMapper m;
        m.map(0, hns(1.0), true, 50.0);
        expect(m.basis() == TimestampMapper::Basis::Mapped, "pilote à 0 : ignoré", 0);
        m.map(hns(50.1), hns(1.0167), false, 50.0167);
        expect(m.basis() == TimestampMapper::Basis::Mapped, "pilote dans le futur (+83 ms) : ignoré", 0);
        m.map(hns(49.0), hns(1.0334), false, 50.0334);
        expect(m.basis() == TimestampMapper::Basis::Mapped, "pilote trop ancien (1 s) : ignoré", 0);
    }

    // --- Choix du mode ------------------------------------------------------------------------------------
    {
        ModeRequest r;
        std::string err;
        expect(parse_mode("", r, &err) && r.automatic(), "mode vide : automatique", 0);
        expect(parse_mode("1920x1080@60", r, &err) && r.width == 1920 && r.height == 1080 && r.fps == 60.0,
               "mode 1920x1080@60", r.fps);
        expect(parse_mode("1280X720", r, &err) && r.width == 1280 && r.height == 720 && r.fps == 0.0,
               "mode 1280X720 sans fréquence", r.width);
        expect(parse_mode("1920x1080@59.94", r, &err) && std::abs(r.fps - 59.94) < 1e-9, "mode 59.94 fps", r.fps);
        int bad = 0;
        for (const char* s : {"1920", "1920x", "x1080", "1920x1080@", "1920x1080@0", "1920x1080@60fps", "abc",
                              "32x32", "1920x1080 "})
            bad += parse_mode(s, r, &err) ? 1 : 0;
        expect(bad == 0 && !err.empty(), "syntaxes invalides refusées (avec message)", bad);
    }
    {
        const std::vector<NativeMode> modes = {
            {3840, 2160, 30.0, Subtype::NV12},  {1920, 1080, 60.0, Subtype::MJPG}, {1920, 1080, 60.0, Subtype::YUY2},
            {1280, 720, 60.0, Subtype::NV12},   {1920, 1080, 30.0, Subtype::NV12}, {1920, 1080, 59.94, Subtype::NV12},
            {2560, 1440, 120.0, Subtype::MJPG}, {32, 32, 60.0, Subtype::NV12},
        };
        const int a = choose_mode(modes, {});
        expect(a == 6, "auto : >= 59 fps puis plus grande résolution (1440p120 MJPEG)", a);
        std::vector<NativeMode> no1440(modes.begin(), modes.begin() + 6);
        const int b = choose_mode(no1440, {});
        expect(b == 2, "auto : 1080p60, non compressé préféré au MJPEG, 60 > 59.94", b);
        ModeRequest r;
        parse_mode("1920x1080@60", r);
        const int c = choose_mode(modes, r);
        expect(c == 2, "demandé 1920x1080@60 : YUY2 (non compressé) plutôt que MJPEG", c);
        parse_mode("1920x1080@30", r);
        expect(choose_mode(modes, r) == 4, "demandé 1920x1080@30", choose_mode(modes, r));
        parse_mode("1920x1080", r);
        expect(choose_mode(modes, r) == 2, "demandé 1920x1080 (toute fréquence) : premier non compressé",
               choose_mode(modes, r));
        {  // --mode WxH sans fréquence : la plus haute fréquence d'abord, quel que soit l'ordre des modes
            const std::vector<NativeMode> m2 = {
                {1920, 1080, 30.0, Subtype::NV12}, {1920, 1080, 59.94, Subtype::NV12}, {1920, 1080, 60.0, Subtype::MJPG},
                {1920, 1080, 25.0, Subtype::YUY2}, {1280, 720, 120.0, Subtype::NV12},
            };
            ModeRequest q;
            parse_mode("1920x1080", q);
            expect(choose_mode(m2, q) == 1, "1920x1080 : 59.94 NV12 (≈ 60, non compressé) plutôt que 30 ou MJPEG 60",
                   choose_mode(m2, q));
            auto m3 = m2;
            m3.push_back({1920, 1080, 120.0, Subtype::MJPG});
            expect(choose_mode(m3, q) == 5, "1920x1080 : 120 fps plutôt que 60", choose_mode(m3, q));
            m3.push_back({1920, 1080, 240.0, Subtype::NV12});
            m3.push_back({1920, 1080, 144.0, Subtype::NV12});
            expect(choose_mode(m3, q) == 6, "1920x1080 : bornée à 144 (240 ~ 144, premier trouvé)", choose_mode(m3, q));
            const std::vector<NativeMode> slow = {{1280, 720, 30.0, Subtype::MJPG}, {1280, 720, 50.0, Subtype::YUY2}};
            parse_mode("1280x720", q);
            expect(choose_mode(slow, q) == 1, "1280x720 sans 60 fps : la plus haute (50)", choose_mode(slow, q));
            parse_mode("1920x1080@60", q);
            const std::vector<NativeMode> m4 = {{1920, 1080, 59.94, Subtype::NV12}, {1920, 1080, 60.0, Subtype::NV12}};
            expect(choose_mode(m4, q) == 1, "1920x1080@60 : 60 exact plutôt que 59.94", choose_mode(m4, q));
        }
        parse_mode("1024x768@60", r);
        expect(choose_mode(modes, r) == -1, "demandé introuvable : -1", choose_mode(modes, r));
        expect(choose_mode({}, {}) == -1, "aucun mode : -1", choose_mode({}, {}));
        expect(choose_mode({{32, 32, 60.0, Subtype::NV12}}, {}) == -1, "mode minuscule ignoré", 0);
        const std::vector<NativeMode> only30 = {{1920, 1080, 30.0, Subtype::NV12}, {1280, 720, 30.0, Subtype::NV12}};
        expect(choose_mode(only30, {}) == 0, "sans 60 fps : la plus grande résolution", choose_mode(only30, {}));
        expect(describe(modes[5]) == "1920x1080 @ 59.94 fps, NV12", "description d'un mode", 0);
    }

    // --- Format de sortie demandé au Source Reader ------------------------------------------------------------
    {
        const auto n = output_order(Subtype::NV12);
        const auto y = output_order(Subtype::YUY2);
        const auto j = output_order(Subtype::MJPG);
        const auto r = output_order(Subtype::RGB32);
        expect(n.size() == 3 && n[0] == Output::NV12 && n[1] == Output::YUY2 && n[2] == Output::RGB32,
               "NV12 natif : NV12, puis YUY2, puis RGB32", 0);
        expect(y[0] == Output::NV12 && j[0] == Output::NV12, "YUY2 / MJPEG : NV12 d'abord (conversion MF)", 0);
        expect(r[0] == Output::RGB32 && r[1] == Output::NV12 && r.size() == 3,
               "RGB32 natif : RGB32 d'abord (pas d'aller-retour YUV)", 0);
        expect(output_from_subtype(Subtype::NV12) == Output::NV12 && output_from_subtype(Subtype::YUY2) == Output::YUY2 &&
                   output_from_subtype(Subtype::RGB32) == Output::RGB32,
               "sous-type livré : NV12 / YUY2 / RGB32 reconnus", 0);
        bool none = true;
        for (Subtype st : {Subtype::P010, Subtype::MJPG, Subtype::H264, Subtype::HEVC, Subtype::Other})
            none &= !output_from_subtype(st).has_value();
        expect(none, "sous-type livré inconnu : refusé (jamais lu comme du NV12)", 0);
        expect(std::string(output_name(Output::YUY2)) == "YUY2", "nom YUY2", 0);
        expect(is_uncompressed(Subtype::P010) && !is_uncompressed(Subtype::MJPG) && !is_uncompressed(Subtype::H264),
               "compressé / non compressé", 0);
    }

    // --- Choix du périphérique -------------------------------------------------------------------------------
    {
        const std::vector<std::wstring> devs = {L"Integrated Webcam", L"OBS Virtual Camera", L"Elgato HD60 X",
                                                L"USB Video"};
        expect(pick_device(devs, L"") == 2, "auto : Elgato avant « USB Video »", pick_device(devs, L""));
        expect(pick_device(devs, L"usb") == 3, "--device usb (casse ignorée)", pick_device(devs, L"usb"));
        expect(pick_device(devs, L"magewell") == -1, "--device absent : -1", pick_device(devs, L"magewell"));
        expect(pick_device({L"Integrated Webcam"}, L"") == 0, "aucun indice : le premier", 0);
        expect(pick_device({}, L"") == -1, "aucun périphérique : -1", 0);
        expect(pick_device({L"Live Gamer Ultra", L"HDMI to USB"}, L"") == 1, "priorité des indices (hdmi < live gamer)",
               pick_device({L"Live Gamer Ultra", L"HDMI to USB"}, L""));
    }

    // --- Doublons sur un signal câble -------------------------------------------------------------------------
    // Jeu 30 fps dans un signal HDMI 60 Hz : les répétitions sont identiques au pixel (métrique 0) ou presque
    // (carte MJPEG), les vraies images vont de petits mouvements (0,6) à de grands. Seuil de base 0.4 (défaut
    // --source capture) : aucune erreur, rythme 2 verrouillé ; même chose en seuil fixe.
    {
        std::mt19937                          rng(99);
        std::uniform_real_distribution<float> mv(0.6f, 12.0f), mjpeg(0.0f, 0.12f);
        for (int variant = 0; variant < 3; ++variant) {
            DupClassifier                 c;
            const DupClassifier::Params prm{0.4f, variant != 2};
            int errors = 0, period = 0;
            for (int i = 0; i < 1200; ++i) {
                const bool  truth = i % 2 == 0;
                const float m     = truth ? mv(rng) : (variant == 1 ? mjpeg(rng) : 0.0f);
                const auto  d     = c.classify(m, prm);
                if (i >= 64) errors += d.unique != truth;
                period = d.period;
            }
            const char* what[] = {"câble exact (0) : erreurs, seuil adaptatif 0.4", "câble MJPEG (0-0.12) : erreurs, adaptatif 0.4",
                                  "câble exact (0) : erreurs, seuil fixe 0.4"};
            expect(errors == 0, what[variant], errors);
            if (variant != 2) expect(period == 2, "câble : rythme 2 verrouillé", period);
        }
    }

    std::printf(g_failures ? "%d ÉCHEC(S)\n" : "TOUT OK\n", g_failures);
    return g_failures ? 1 : 0;
}
