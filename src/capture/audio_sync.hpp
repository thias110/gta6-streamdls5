#pragma once
// Relais du son entre deux horloges matérielles (entrée de la carte de capture -> sortie audio par défaut).
// Logique pure, sans Windows : testée seule (tests/audio_sync_test.cpp), y compris hors Windows.
//
// Pourquoi : la carte et la sortie audio ont chacune leur quartz. Même à « 48 kHz » des deux côtés, l'une va
// un peu plus vite que l'autre (quelques ppm à quelques centaines de ppm) : sans correction, la file se vide
// (trous) ou se remplit (retard qui grimpe, puis paquets jetés = coupures audibles).
//
// Ce qui est fait ici : une file intermédiaire (Pump) dont la latence totale (file + tampon de sortie) est
// tenue autour d'une cible (~25 ms + le retard voulu, voir --audio-delay) par un rééchantillonnage linéaire
// dont le rapport est corrigé en douceur (DriftController : PI, borné à ±kMaxPpm). On ne jette des trames
// qu'en cas d'écart énorme (> max_excess_ms, ex. sortie bloquée), et on n'ajoute du silence d'un coup qu'en
// cas de gros déficit (> max_deficit_ms, entrée coupée puis reprise). Les changements du retard voulu sont appliqués d'un coup (silence ajouté ou
// trames jetées), comme avant, sans perturber l'asservissement.

#include "audio_delay.hpp"

#include <cstdint>
#include <vector>

namespace audio_sync {

// Asservissement de la dérive : erreur de latence (ms, positive = trop de son en attente) -> correction du
// rapport de rééchantillonnage en ppm (positive = on consomme l'entrée plus vite). L'erreur est lissée
// (moyenne exponentielle, kTauS) : la mesure brute oscille de la taille d'un paquet (~10 ms).
class DriftController {
public:
    static constexpr double kMaxPpm = 500.0;  // borne de la correction (au-delà : réamorçage ou trames jetées)
    static constexpr double kKp     = 100.0;  // ppm par ms d'erreur (constante de temps ~10 s)
    static constexpr double kKi     = 5.0;    // ppm par ms d'erreur et par seconde (rattrape la dérive fixe)
    static constexpr double kTauS   = 1.0;    // lissage de la mesure

    double update(double error_ms, double dt_s);  // renvoie la correction en ppm
    void   reset();
    void   rebase() { init_ = false; }  // oublie la mesure lissée (après un saut voulu), garde la dérive estimée
    double ppm() const { return ppm_; }
    double filtered_ms() const { return filt_; }

private:
    bool   init_  = false;
    double filt_  = 0.0;
    double integ_ = 0.0;  // ppm
    double ppm_   = 0.0;
};

// Rééchantillonneur linéaire, trames float entrelacées. step = trames d'entrée consommées par trame de
// sortie (in_rate / out_rate, corrigé de la dérive). Garde la dernière trame et la phase entre deux appels :
// le découpage en paquets ne change pas le résultat.
class LinearResampler {
public:
    explicit LinearResampler(uint32_t channels = 2);
    void     reset();
    uint32_t channels() const { return ch_; }
    // Trames d'entrée nécessaires pour produire n_out trames avec ce pas.
    uint32_t input_needed(uint32_t n_out, double step) const;
    // Plus grand n_out produisible avec n_in trames d'entrée.
    uint32_t output_possible(uint32_t n_in, double step) const;
    // Produit n_out trames (in doit en contenir input_needed(n_out, step)) ; renvoie les trames consommées.
    uint32_t process(const float* in, uint32_t n_in, float* out, uint32_t n_out, double step);

private:
    uint32_t           ch_;
    double             frac_ = 0.0;  // position de la prochaine sortie après la trame `prev_`, dans [0, 1)
    std::vector<float> prev_;
};

struct PumpConfig {
    uint32_t in_rate       = 48000;
    uint32_t out_rate      = 48000;
    uint32_t channels      = 2;
    float    jitter_ms     = 5.0f;   // marge gardée dans la file au-delà du retard voulu (paquets irréguliers)
    float    out_fill_ms   = 20.0f;  // remplissage visé du tampon de sortie (protège des famines)
    float    max_excess_ms = 60.0f;  // au-delà de cible + ceci, on jette (sortie bloquée, à-coup du pilote)
    float    max_deficit_ms = 20.0f; // en deçà de cible - ceci, on ajoute du silence (entrée coupée puis reprise)
};

class Pump {
public:
    explicit Pump(const PumpConfig& cfg);

    const PumpConfig& config() const { return cfg_; }
    // Trames capturées (float entrelacées, cfg.channels) ; nullptr = silence.
    void push(const float* frames, uint32_t n);
    // Remplit `out` (au plus `room` trames) pour ramener le tampon de sortie (`padding` trames déjà en
    // attente côté sortie) vers out_fill_ms. delay_ms : retard voulu (--audio-delay). now_s : horloge
    // monotone (secondes). Renvoie le nombre de trames écrites dans `out` (silence compris).
    // Limite connue : `padding` avance par périodes de ~10 ms du moteur audio ; sa phase par rapport aux paquets
    // capturés glisse lentement avec la dérive, ce qui biaise la mesure de quelques ms et fait osciller
    // lentement la correction (bornée à kMaxPpm, soit < 1 cent : inaudible). En moyenne elle vaut la dérive.
    uint32_t pull(float* out, uint32_t room, uint32_t padding, float delay_ms, double now_s);

    // Statistiques (trames d'entrée sauf mention contraire).
    uint64_t relayed() const { return relayed_; }        // trames de son réel écrites (sortie)
    uint64_t dropped() const { return dropped_; }        // trames d'entrée jetées (écart énorme, retard réduit)
    uint64_t silence() const { return silence_; }        // trames de silence écrites (amorçage, retard accru)
    uint64_t reprimes() const { return reprimes_; }      // gros déficits (entrée en retard) -> silence ajouté
    double   ppm() const { return ctrl_.ppm(); }
    double   latency_error_ms() const { return ctrl_.filtered_ms(); }
    uint32_t queued() const { return fifo_.frames(); }  // trames en attente dans la file
    bool     started() const { return started_; }
    // Latence visée (ms) hors retard voulu : jitter + remplissage de la sortie.
    float    base_latency_ms() const { return cfg_.jitter_ms + cfg_.out_fill_ms; }

private:
    double in_per_out() const { return double(cfg_.in_rate) / double(cfg_.out_rate); }

    PumpConfig             cfg_;
    audio_delay::FrameFifo fifo_;
    LinearResampler        rs_;
    DriftController        ctrl_;
    std::vector<float>     tmp_;
    bool                   started_       = false;
    bool                   have_now_      = false;
    double                 last_now_      = 0.0;
    uint32_t               delay_in_      = 0;  // retard appliqué (trames d'entrée)
    uint32_t               pending_sil_   = 0;  // silence à écrire (trames de sortie) suite à un retard accru
    uint64_t               relayed_ = 0, dropped_ = 0, silence_ = 0, reprimes_ = 0;
};

// Réouverture après une erreur du périphérique : 0,5 s, 1 s, 2 s, 4 s puis 5 s entre deux essais ; abandon
// après kMaxRetries essais ratés d'affilée (~2 min).
constexpr int kMaxRetries = 30;
double        retry_delay_s(int attempt);

}  // namespace audio_sync
