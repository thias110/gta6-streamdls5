#pragma once
// Amélioration d'image (--enhance) : réglages, bornes, lissage des changements de réglage et références CPU des
// passes GPU (kDenoise dans src/gpu/shaders.hpp). Sans en-tête Windows : testé sous Linux (tests/enhance_test.cpp).
// Les références CPU suivent les shaders formule pour formule (mêmes constantes) : si l'un change, l'autre aussi.
#include <cstdint>
#include <vector>

namespace enhance {

// Réglages demandés. Chaque effet se désactive séparément : intensité 0 (saturation 1 = neutre).
struct Params {
    bool  enabled    = false;  // --enhance on|off ; off (défaut) : pipeline strictement inchangé
    float denoise    = 0.35f;  // étage A (avant l'upscale) : débruitage / déblocking bilatéral, 0..1
    float sharpen    = 0.35f;  // étage B : netteté adaptative sans dépassement, 0..1
    float clarity    = 0.20f;  // étage B : contraste local à grand rayon (luminance), 0..1
    float tone       = 0.20f;  // étage B : courbe de tons douce en S, 0..1
    float saturation = 1.05f;  // étage B : 0.5..1.5, 1 = neutre ; au-dessus de 1, façon « vibrance »
    bool  dither     = true;   // étage B : bruit triangulaire de +-1 niveau sur les pixels modifiés (anti-banding)
};

inline constexpr float kSatMin = 0.5f, kSatMax = 1.5f;

// Bornes (0..1, saturation 0.5..1.5) ; une valeur non finie reprend la valeur par défaut.
Params clamp_params(const Params& p);

// ----- Étage A : filtre bilatéral 5x5 (ps_denoise) -----------------------------------------------------------
// poids(dx, dy) = exp(-(dx² + dy²) / (2 sigma_s²) - |c(p+d) - c(p)|² / 3 / (2 sigma_r²)), couleurs 0..1 ;
// sortie = lerp(c, moyenne pondérée, mix). sigma_r (seuil de « contour ») couvre le bruit et les marches de blocs
// d'un flux H.264/HEVC réencodé (quelques niveaux sur 255) sans lisser les vrais contours (dizaines de niveaux).
struct DenoiseCoeffs {
    float sigma_r = 0.0f;  // écart de couleur (0..1) toléré
    float sigma_s = 1.0f;  // étalement spatial (pixels)
    float mix     = 0.0f;  // part du filtré dans la sortie
};
DenoiseCoeffs denoise_coeffs(float strength);

// Image 8 bits à 4 composantes dans l'ordre de la texture (B, G, R, A) : la passe GPU travaille en BGRA8.
struct Image8 {
    int                  w = 0, h = 0;
    std::vector<uint8_t> px;
    uint8_t*       at(int x, int y) { return px.data() + (size_t(y) * size_t(w) + size_t(x)) * 4; }
    const uint8_t* at(int x, int y) const { return px.data() + (size_t(y) * size_t(w) + size_t(x)) * 4; }
};

// Référence CPU de ps_denoise (bords : texels répétés, comme clamp() dans le shader). Alpha forcé à 255.
Image8 denoise_ref(const Image8& src, float strength);

// ----- Lissage des réglages --------------------------------------------------------------------------------
// Les changements (curseurs de l'overlay, activation) sont amenés en douceur (constante de temps tau) pour éviter
// un saut visible d'une image à l'autre. Premier appel, ou pause > 1 s : valeurs prises telles quelles.
class Smoother {
public:
    // Réglages effectifs à l'instant `now` (secondes). Désactivé (enabled = false) quand la cible est off et que
    // la transition est terminée : le pipeline reprend alors exactement le chemin sans amélioration.
    Params update(const Params& target, double now, float tau = 0.15f);
    void   reset() { init_ = false; }

private:
    Params cur_;
    float  mix_  = 0.0f;  // 0 = sans effet, 1 = réglages pleins
    bool   init_ = false;
    double last_ = 0.0;
};

}  // namespace enhance
