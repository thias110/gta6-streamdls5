#pragma once
#include "../common.hpp"

#include <mutex>
#include <thread>

std::vector<std::wstring> list_audio_inputs();

// Relais audio basse latence : entrée audio de la carte de capture -> sortie audio par défaut (WASAPI
// partagé, piloté par événement). Garde ~10 à 60 ms en tampon et jette l'excédent pour ne jamais
// accumuler de retard sur l'image.
class AudioPassthrough {
public:
    AudioPassthrough();
    ~AudioPassthrough();

    // needle : sous-chaîne du nom de l'entrée audio ; vide = déduite du nom de la carte vidéo.
    bool start(const std::wstring& needle, const std::string& video_device_name, float volume);
    void stop();
    void set_volume(float v);

    bool        running() const { return running_; }
    std::string name() const;
    float       buffered_ms() const { return buffered_ms_; }
    uint64_t    frames_relayed() const { return frames_; }
    uint64_t    frames_dropped() const { return dropped_; }

private:
    void loop();

    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread           thread_;
    std::atomic<bool>     running_{false};
    std::atomic<float>    buffered_ms_{0.0f};
    std::atomic<uint64_t> frames_{0}, dropped_{0};
    std::string           name_;
};
