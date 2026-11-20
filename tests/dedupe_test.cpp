// Tests du classement doublon / nouvelle image (logique CPU pure, sans GPU).
// Compilation autonome : g++ -std=c++20 -Isrc tests/dedupe_test.cpp src/pipeline/dedupe.cpp
#include "pipeline/dedupe.hpp"

#include <cmath>
#include <cstdio>
#include <random>

namespace {
int g_failures = 0;

void expect(bool ok, const char* what, double value) {
    std::printf("%s  %-60s %.3f\n", ok ? "OK   " : "ÉCHEC", what, value);
    if (!ok) ++g_failures;
}

struct Rng {
    std::mt19937 g{1234};
    float uni(float a, float b) { return std::uniform_real_distribution<float>(a, b)(g); }
};

// Flux 60 fps d'un jeu à 60/p fps : une nouvelle image toutes les p captures.
// Renvoie le taux d'erreur après `warmup` captures.
template <class NewFn, class DupFn>
double run_cadence(DupClassifier& c, const DupClassifier::Params& prm, int p, int n, int warmup, NewFn nf, DupFn df,
                   int* period_out = nullptr) {
    int errors = 0, counted = 0;
    for (int i = 0; i < n; ++i) {
        const bool  truth = i % p == 0;
        const float m     = truth ? nf() : df();
        const auto  d     = c.classify(m, prm);
        if (i >= warmup) {
            ++counted;
            errors += d.unique != truth;
        }
        if (period_out) *period_out = d.period;
    }
    return counted ? double(errors) / counted : 0.0;
}
}  // namespace

int main() {
    Rng rng;
    const DupClassifier::Params adaptive{1.2f, true}, fixed{1.2f, false};

    {  // 1. Réencodage très bruité : le bruit des répétitions dépasse le seuil de base.
        DupClassifier c, f;
        auto nf = [&] { return rng.uni(5.0f, 20.0f); };
        auto df = [&] { return rng.uni(1.5f, 2.6f); };
        const double e_fixed = run_cadence(f, fixed, 2, 800, 100, nf, df);
        int          period  = 0;
        const double e_adapt = run_cadence(c, adaptive, 2, 800, 100, nf, df, &period);
        expect(e_fixed > 0.3, "bruit fort : le seuil fixe se trompe (attendu)", e_fixed);
        expect(e_adapt < 0.01, "bruit fort : seuil adaptatif, taux d'erreur", e_adapt);
        expect(period == 2, "bruit fort : rythme verrouillé sur 2", period);
        expect(c.threshold() > 2.6f && c.threshold() < 5.0f, "bruit fort : seuil appris entre bruit et signal",
               c.threshold());
    }
    {  // 2. Panoramique lent : les vraies images bougent à peine (sous le seuil de base).
        DupClassifier c;
        auto nf = [&] { return rng.uni(0.7f, 1.1f); };
        auto df = [&] { return rng.uni(0.1f, 0.3f); };
        const double e = run_cadence(c, adaptive, 2, 800, 100, nf, df);
        expect(e < 0.01, "mouvement lent : seuil adaptatif, taux d'erreur", e);
    }
    {  // 3. Contenu 60 fps (menus, cinématiques) : tout est nouveau, aucune bimodalité.
        DupClassifier c;
        int errors = 0;
        for (int i = 0; i < 1000; ++i) {
            const float m = 1.5f * std::pow(20.0f, rng.uni(0.0f, 1.0f));  // log-uniforme 1.5 .. 30
            errors += !c.classify(m, adaptive).unique;
        }
        expect(errors <= 5, "60 fps log-uniforme 1.5-30 : images perdues (sur 1000)", errors);
        expect(c.period() == 0, "60 fps : pas de rythme verrouillé", c.period());
    }
    {  // 4. Image fixe : tout est répétition.
        DupClassifier c;
        int errors = 0;
        for (int i = 0; i < 1000; ++i) errors += c.classify(rng.uni(0.1f, 0.5f), adaptive).unique;
        expect(errors == 0, "image fixe : fausses nouvelles images (sur 1000)", errors);
    }
    {  // 5. Rythme verrouillé : une nouvelle image ambiguë (sous le seuil) est quand même reconnue.
        DupClassifier c;
        run_cadence(c, adaptive, 2, 200, 0, [&] { return rng.uni(4.0f, 12.0f); }, [&] { return rng.uni(0.2f, 0.6f); });
        const float thr = c.threshold();
        // 200 captures (indices 0..199) : la dernière était une répétition, la suivante doit être nouvelle.
        const auto d = c.classify(thr * 0.7f, adaptive);
        expect(d.unique && d.predicted, "rythme 2 : image ambiguë (0.7 x seuil) classée nouvelle", d.unique);
        const auto r = c.classify(thr * 1.4f, adaptive);  // ambiguë mais à la place d'une répétition
        expect(!r.unique && r.predicted, "rythme 2 : répétition ambiguë (1.4 x seuil) classée doublon", r.unique);
        c.classify(rng.uni(4.0f, 12.0f), adaptive);   // nouvelle image normale
        const auto big = c.classify(30.0f, adaptive);  // à la place d'une répétition  // métrique franche : le rythme ne la contredit pas
        expect(big.unique && !big.predicted, "rythme 2 : métrique franche hors rythme gardée", big.unique);
    }
    {  // 6. Saccade du jeu (A A A B B) : pas d'erreur sur des métriques franches, re-verrouillage ensuite.
        DupClassifier c;
        auto nf = [&] { return rng.uni(4.0f, 12.0f); };
        auto df = [&] { return rng.uni(0.2f, 0.6f); };
        run_cadence(c, adaptive, 2, 200, 0, nf, df);
        int errors = 0;
        const bool seq[] = {false, false, true, false, true, false, true, false};  // répétition en trop puis reprise
        for (bool truth : seq) errors += c.classify(truth ? nf() : df(), adaptive).unique != truth;
        expect(errors == 0, "saccade A A A B B : erreurs", errors);
        int period = 0;
        run_cadence(c, adaptive, 2, 40, 0, nf, df, &period);
        expect(period == 2, "saccade : rythme 2 retrouvé", period);
    }
    {  // 7. Jeu à 20 fps dans un flux à 60 : rythme 3.
        DupClassifier c;
        int period = 0;
        const double e = run_cadence(c, adaptive, 3, 600, 100, [&] { return rng.uni(3.0f, 10.0f); },
                                     [&] { return rng.uni(0.2f, 0.6f); }, &period);
        expect(e < 0.01, "20 fps : taux d'erreur", e);
        expect(period == 3, "20 fps : rythme verrouillé sur 3", period);
    }
    {  // 8. Mode fixe (--no-adaptive-dedupe) : exactement l'ancien comportement.
        DupClassifier c;
        int diff = 0;
        for (int i = 0; i < 500; ++i) {
            const float m = rng.uni(0.0f, 3.0f);
            const auto  d = c.classify(m, fixed);
            diff += d.unique != (m > 1.2f) || d.period != 0 || d.predicted;
        }
        expect(diff == 0, "seuil fixe : écarts avec metric > seuil", diff);
    }

    std::printf(g_failures ? "%d ÉCHEC(S)\n" : "TOUT OK\n", g_failures);
    return g_failures ? 1 : 0;
}
