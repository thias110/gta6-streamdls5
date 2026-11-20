#pragma once
// Classement des captures en « nouvelle image du jeu » / « répétition », sans dépendance Windows
// (testable seul, voir tests/dedupe_test.cpp).
//
// - Seuil adaptatif : la métrique (plus forte différence moyenne d'une tuile, 0-255) est bimodale
//   quand le jeu tourne moins vite que le flux (bruit de réencodage des répétitions d'un côté,
//   vraies images de l'autre). On sépare les deux populations sur une fenêtre glissante (séparation
//   d'Otsu sur le logarithme) et le seuil glisse vers le milieu (géométrique) de l'écart entre les
//   deux, borné autour du seuil de base. Sans deux populations nettes, il revient vers la base.
// - Verrou de rythme : si les nouvelles images tombent régulièrement toutes les 2 (jeu à 30 fps
//   dans un flux à 60) ou 3 captures, on prédit la suivante ; la prédiction ne tranche que les
//   métriques ambiguës (proches du seuil), une métrique franche l'emporte toujours.
// Idées inspirées de SmoovPlay (MIT, framegen/cadence.py) ; implémentation propre à rpenhance.

#include <array>
#include <cstdint>

class DupClassifier {
public:
    struct Params {
        float base_threshold = 1.2f;  // --dup-threshold
        bool  adaptive       = true;  // false : seuil fixe = base, pas de verrou de rythme
    };
    struct Decision {
        bool  unique    = true;
        float threshold = 0.0f;   // seuil effectivement appliqué
        int   period    = 0;      // rythme verrouillé (2 ou 3), 0 = libre
        bool  predicted = false;  // la décision vient du rythme (métrique ambiguë)
    };

    static constexpr int kWindow    = 64;  // captures pour le seuil
    static constexpr int kMinSplit  = 12;  // échantillons minimum de chaque côté de la séparation
    static constexpr int kRhythm    = 16;  // captures examinées pour le rythme
    static constexpr int kMaxMisses = 1;   // écarts tolérés au rythme dans cette fenêtre

    void     reset();
    Decision classify(float metric, const Params& p);

    float threshold() const { return thr_; }
    int   period() const { return period_; }

private:
    void update_threshold(float base);
    void update_rhythm();
    bool predicted_new() const;

    std::array<float, kWindow>   metrics_{};
    int                          n_metrics_ = 0, pos_ = 0;
    std::array<uint8_t, kRhythm> calls_{};  // 1 = nouvelle image ; calls_[(count_ - 1) % kRhythm] = la dernière
    uint64_t                     count_    = 0;  // captures classées depuis reset()
    uint64_t                     last_new_ = 0;  // index (1-based) de la dernière nouvelle image, 0 = aucune
    float                        thr_      = -1.0f;
    int                          period_   = 0;
};
