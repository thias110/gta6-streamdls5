#pragma once
#include "config.hpp"

// Mesure sans fenêtre ni PS5 : détection des doublons, qualité de l'interpolation (PSNR contre la
// vraie frame intermédiaire de la mire), coût GPU, disponibilité de RTX VSR. Écrit des BMP dans
// <exe>\selftest\. Code de sortie 0 si tout passe.
int run_selftest(const Config& cfg);
