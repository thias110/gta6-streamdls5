#pragma once
#include "config.hpp"

// Mesure sans fenêtre ni PS5 : détection des doublons, qualité de l'interpolation (PSNR contre la
// vraie frame intermédiaire de la mire), coût GPU, disponibilité de RTX VSR. Écrit des BMP dans
// <exe>\selftest\. Code de sortie 0 si tout passe.
int run_selftest(const Config& cfg);

// Contrôle du frame tap sans fenêtre : lit les frames émises par tests/tap_writer (sur --tap-name), relit chaque
// texture BGRA convertie et vérifie numéro de frame, couleurs, drapeaux, sessions, arrêt et redémarrage de
// l'émetteur. Code de sortie 0 si tout passe, 1 sinon (voir src/selftest_tap.cpp).
int run_selftest_tap(const Config& cfg);
