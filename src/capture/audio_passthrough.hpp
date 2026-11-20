#pragma once
#include "../common.hpp"

#include <thread>

std::vector<std::wstring> list_audio_inputs();

// Relais audio basse latence : entrée audio de la carte de capture -> sortie audio par défaut (WASAPI
// partagé, piloté par événement). Les deux périphériques ont chacun leur horloge : une file intermédiaire et
// un rééchantillonnage linéaire légèrement corrigé (audio_sync.hpp) tiennent la latence autour de ~25 ms sans
// jeter de paquets en régime normal. set_delay_ms() ajoute un retard voulu (en plus) pour suivre le retard
// d'affichage imposé par l'interpolation (voir audio_delay.hpp, --audio-delay).
class AudioPassthrough {
public:
    AudioPassthrough();
    ~AudioPassthrough();

    // needle : sous-chaîne du nom de l'entrée audio ; vide = déduite du nom de la carte vidéo.
    bool start(const std::wstring& needle, const std::string& video_device_name, float volume);
    void stop();
    void set_volume(float v);  // thread-safe : appliqué par le thread du relais
    // En cas d'erreur WASAPI (périphérique débranché, AUDCLNT_E_DEVICE_INVALIDATED...), le thread du relais
    // ferme les flux, journalise une fois et rouvre avec une temporisation croissante ; il abandonne
    // proprement (running() devient faux) après audio_sync::kMaxRetries échecs.
    // Retard voulu du son (ms, 0..250), lu par le thread du relais à chaque paquet. Thread-safe.
    void set_delay_ms(float ms) { delay_ms_ = ms; }
    float delay_ms() const { return delay_ms_; }
    float applied_delay_ms() const { return applied_delay_ms_; }  // trames réellement en attente (ms)

    bool        running() const { return running_; }
    std::string name() const;
    float       buffered_ms() const { return buffered_ms_; }
    uint64_t    frames_relayed() const { return frames_; }
    uint64_t    frames_dropped() const { return dropped_; }
    float       drift_ppm() const { return drift_ppm_; }  // correction de dérive appliquée (ppm)

private:
    bool    open(bool verbose);
    HRESULT relay();
    void    loop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};
    std::atomic<float>    buffered_ms_{0.0f}, drift_ppm_{0.0f};
    std::atomic<float>    delay_ms_{0.0f}, applied_delay_ms_{0.0f};
    std::atomic<uint64_t> frames_{0}, dropped_{0};
    std::string           name_;       // fixé par start() (lu par l'UI)
    std::string           impl_name_;  // entrée effectivement ouverte (thread du relais après start)
    std::wstring          needle_;
    std::string           video_name_;
    std::atomic<float>    volume_{1.0f};
    std::atomic<bool>     volume_dirty_{false};
};
