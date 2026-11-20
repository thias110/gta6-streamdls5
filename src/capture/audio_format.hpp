#pragma once
// Formats audio du relais (entrée de la carte -> sortie par défaut) : décodage en float, mixage des canaux
// (downmix 5.1/7.1 -> stéréo, mono <-> stéréo), encodage vers le format de sortie, et ordre des formats de sortie
// à essayer. Logique pure, sans Windows : testée seule (tests/audio_sync_test.cpp).
//
// Pourquoi : le format de mixage de l'entrée (souvent 48 kHz float, parfois 5.1/7.1 en PCM) était passé tel
// quel à la sortie ; un format multicanal peut y être refusé. On essaie maintenant une liste de formats
// simples (float puis int16, mêmes canaux si la sortie a les mêmes, sinon stéréo), puis en dernier le format
// de mixage exact de la sortie (fréquence comprise : le rééchantillonneur du relais s'en charge).

#include <cstdint>
#include <string>
#include <vector>

namespace audio_format {

enum class Sample { Unknown, Int16, Int24, Int32, Float32 };
const char* sample_name(Sample s);
uint32_t    sample_bytes(Sample s);

struct Pcm {
    uint32_t rate     = 0;
    uint16_t channels = 0;
    Sample   sample   = Sample::Unknown;
    uint32_t mask     = 0;  // positions des canaux (SPEAKER_*), 0 = défaut pour ce nombre de canaux
    uint32_t block() const { return uint32_t(channels) * sample_bytes(sample); }
    bool     valid() const { return rate > 0 && channels > 0 && sample != Sample::Unknown; }
};
std::string describe(const Pcm& p);  // "48000 Hz, 2 canaux, float32"

// Masque par défaut (conventions WAVEFORMATEXTENSIBLE) : 1 = FC, 2 = FL FR, 6 = 5.1, 8 = 7.1...
uint32_t default_mask(uint16_t channels);

// Matrice de mixage dst x src (ligne = canal de sortie). Identité si les canaux sont les mêmes ; mono ->
// stéréo recopié ; vers stéréo : gauche/droite + centre et arrières à -3 dB, LFE ignoré, ligne normalisée
// (pas d'écrêtage) ; vers mono : moyenne hors LFE.
std::vector<float> mix_matrix(uint16_t src_channels, uint32_t src_mask, uint16_t dst_channels);
bool               is_identity(const std::vector<float>& m, uint16_t src_channels, uint16_t dst_channels);

// Décode `frames` trames du format `in` et les mixe vers dst_channels (float entrelacé). scratch : tampon
// réutilisable (évite une allocation par paquet).
void to_float(const uint8_t* src, uint32_t frames, const Pcm& in, uint16_t dst_channels,
              const std::vector<float>& matrix, float* dst, std::vector<float>& scratch);
// Encode des trames float (out.channels canaux) dans le format `out` (écrêtage à [-1, 1] pour les entiers).
void from_float(const float* src, uint32_t frames, const Pcm& out, uint8_t* dst);

// Formats de sortie à essayer, dans l'ordre. autoconvert : le moteur audio convertit fréquence et canaux
// (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM) ; sinon le format est exactement le format de mixage de la sortie.
struct Candidate {
    Pcm  fmt;
    bool autoconvert = true;
};
std::vector<Candidate> output_candidates(const Pcm& in, const Pcm& out_mix);

}  // namespace audio_format
