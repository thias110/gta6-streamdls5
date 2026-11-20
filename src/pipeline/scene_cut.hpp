#pragma once
// Détection de changement de plan, partie CPU (sans dépendance Windows, testable seule) :
// interprétation des sommes produites par cs_cut_stats et décision.
//
// En plus de l'ancien critère (différence moyenne |dY| > cut_threshold), deux indices qui ne
// dépendent pas de la luminosité globale, pour les coupures entre plans sombres où la différence
// brute reste faible :
//  - corrélation de la luminance par blocs (~32x32 px), meilleure de 9 décalages d'un bloc pour
//    qu'un mouvement de caméra ordinaire ne passe pas pour une coupure ;
//  - distance entre histogrammes de luminance (variation totale, 0..1).
// La différence moyenne seule ne suffit plus quand la corrélation montre un simple déplacement
// (voir CutParams::corr_same). Idées inspirées de SmoovPlay (MIT, framegen/cadence.py) ; implémentation propre à rpenhance.

#include <cstdint>

inline constexpr int kCutBins   = 32;
inline constexpr int kCutShifts = 9;  // décalages (-1..1, -1..1) d'un bloc

struct CutStatsRaw {
    float    n = 0.0f, sa = 0.0f, sb = 0.0f, saa = 0.0f, sbb = 0.0f;
    float    sab[kCutShifts]{};
    uint32_t ha[kCutBins]{}, hb[kCutBins]{};
};

struct CutFeatures {
    bool  valid = false;
    float corr  = 1.0f;  // meilleure corrélation des blocs (-1..1) ; 1 si les deux images sont unies
    float hist  = 0.0f;  // distance d'histogrammes (0 = identiques, 1 = disjoints)
};

struct CutParams {
    bool  enabled     = true;   // indices corrélation/histogramme (le critère de différence moyenne reste)
    float mean_max    = 40.0f;  // ancien critère : |dY| moyen (0-255) au-delà duquel c'est une coupure
    float corr_max    = 0.35f;  // coupure si corrélation < corr_max ET histogrammes éloignés...
    float hist_min    = 0.15f;
    float corr_strong = 0.10f;  // ...ou si corrélation < corr_strong quelle que soit la luminosité
    // Garde-fou du critère de différence moyenne : un panoramique rapide sur un décor très contrasté
    // dépasse mean_max (CI, selftest WARP, mire x2.5 : corr 0.94, histo 0.24 -> faux « coupure »).
    // Si la corrélation reste >= corr_same ET les histogrammes proches (<= hist_same), le contenu est
    // celui de l'image précédente déplacé d'au plus un bloc : même plan, la différence moyenne est ignorée.
    // La distance d'histogrammes garde les coupures entre plans de disposition proche mais de tonalité
    // différente (corrélation haute, histogrammes éloignés).
    float corr_same   = 0.85f;
    float hist_same   = 0.35f;
};

CutFeatures cut_features(const CutStatsRaw& r);
bool        is_scene_cut(float mean_diff, const CutFeatures& f, const CutParams& p);
