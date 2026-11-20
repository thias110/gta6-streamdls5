#pragma once
// Retard du son relayé (carte de capture -> sortie audio) pour rester synchrone avec l'image quand
// l'interpolation retarde l'affichage. Logique pure, sans Windows : testée seule (tests/audio_delay_test.cpp).
//
// Pourquoi : avec la frame generation, l'image affichée est celle de l'instant (maintenant - retard), le retard
// valant ~1 intervalle entre images uniques + marges (PipelineInfo::delay_ms). Le son, relayé tel quel, sortirait
// donc en avance d'autant. En passthrough (--no-fg), l'image part dès son arrivée : rien à compenser.
//
// Ce qui est compensé en mode auto : delay_ms - arrival_ms, c'est-à-dire le retard que l'interpolation ajoute
// par rapport au passthrough. Le retard d'arrivée des images (arrival_ms : capture -> soumission, USB et pilote)
// existe aussi côté son (même carte) et n'est pas ajouté. Les latences propres du chemin audio (WASAPI) et vidéo
// (présentation, écran) ne sont pas mesurées : `--audio-delay <ms>` permet de régler à la main.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace audio_delay {

enum class Mode { Auto, Fixed, Off };
struct Setting {
    Mode  mode = Mode::Auto;
    float ms   = 0.0f;  // Mode::Fixed uniquement
};

constexpr float kMaxMs = 250.0f;  // borne de sécurité (le retard d'interpolation reste bien en dessous)

// "auto", "off", ou un nombre de millisecondes 0..250 ("40", "37.5"). false + message sinon.
bool parse(std::wstring_view text, Setting& out, std::string* error = nullptr);
std::string describe(const Setting& s);  // "auto", "off", "40 ms"

// Retard ajouté par l'interpolation par rapport au passthrough, borné à [0, kMaxMs] ; 0 sans frame generation.
float interpolation_extra_ms(bool frame_gen, float delay_ms, float arrival_ms);
// Retard du son voulu pour un réglage donné.
float target_ms(const Setting& s, bool frame_gen, float delay_ms, float arrival_ms);

// Lissage : le retard du son ne change que par sauts d'au moins kStepMs, au plus une fois par kHoldS (chaque
// changement coûte un petit trou ou un petit saut audible) ; retour à 0 immédiat (FG coupée, mode off).
class Tracker {
public:
    static constexpr float  kStepMs = 8.0f;
    static constexpr double kHoldS  = 0.5;

    float update(float target_ms, double now_s);
    float current() const { return cur_; }

private:
    bool   init_        = false;
    float  cur_         = 0.0f;
    double last_change_ = 0.0;
};

uint32_t ms_to_frames(float ms, uint32_t sample_rate);

// Décision pour un passage de la boucle du relais : `fifo` trames en attente (retard logiciel), `target` trames
// à y garder (le retard voulu), `room` place libre dans le tampon de sortie, `padding` trames déjà en sortie.
//  - plus de target + max_excess en attente (le son a pris du retard) : on jette jusqu'à revenir à target ;
//  - sortie déjà chargée au-delà de max_padding : on jette ce qui serait parti (on rattrape) ;
//  - sinon on écrit ce qui dépasse target, dans la limite de room.
struct Plan {
    uint32_t write = 0;  // trames à écrire en sortie (les plus anciennes)
    uint32_t drop  = 0;  // trames à jeter avant l'écriture (les plus anciennes)
};
Plan plan(uint32_t fifo, uint32_t target, uint32_t room, uint32_t padding, uint32_t max_excess, uint32_t max_padding);

// File de trames audio (taille de trame fixe, octets opaques), anneau qui grandit au besoin.
class FrameFifo {
public:
    explicit FrameFifo(uint32_t block_bytes = 4) : block_(block_bytes ? block_bytes : 1) {}
    void     reset(uint32_t block_bytes);
    void     push(const uint8_t* data, uint32_t frames);  // data == nullptr : silence
    uint32_t pop(uint8_t* dst, uint32_t frames);          // renvoie le nombre de trames copiées
    uint32_t peek(uint8_t* dst, uint32_t frames) const;   // comme pop, sans retirer les trames
    void     drop(uint32_t frames);
    uint32_t frames() const { return uint32_t(size_ / block_); }

private:
    void grow(size_t need);

    std::vector<uint8_t> buf_;
    size_t               head_ = 0, size_ = 0;  // octets
    uint32_t             block_;
};

}  // namespace audio_delay
