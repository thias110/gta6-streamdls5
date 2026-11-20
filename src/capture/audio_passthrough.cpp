#include "audio_passthrough.hpp"
#include "audio_delay.hpp"
#include "audio_sync.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>
// Définit PKEY_Device_FriendlyName dans cette unité (pas de .lib qui la fournit).
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstring>
#include <cwctype>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

namespace {

struct Endpoint {
    ComPtr<IMMDevice> device;
    std::wstring      name;
};

std::vector<Endpoint> capture_endpoints() {
    ComPtr<IMMDeviceEnumerator> en;
    check_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)),
             "MMDeviceEnumerator");
    ComPtr<IMMDeviceCollection> coll;
    check_hr(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &coll), "EnumAudioEndpoints");
    UINT n = 0;
    coll->GetCount(&n);
    std::vector<Endpoint> out;
    for (UINT i = 0; i < n; ++i) {
        Endpoint e;
        if (FAILED(coll->Item(i, &e.device))) continue;
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(e.device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) e.name = pv.pwszVal;
            PropVariantClear(&pv);
        }
        out.push_back(std::move(e));
    }
    return out;
}

// Mots du nom de la carte vidéo utiles pour retrouver son entrée audio ("Elgato 4K X" -> elgato, 4k).
std::vector<std::wstring> words_of(const std::wstring& s) {
    std::vector<std::wstring> w;
    std::wstring cur;
    for (wchar_t c : to_lower(s) + L" ") {
        if (iswalnum(c)) cur += c;
        else {
            if (cur.size() >= 3) w.push_back(cur);
            cur.clear();
        }
    }
    return w;
}

// Échantillons d'un format PCM / float partagé (WAVE_FORMAT_PCM, IEEE_FLOAT ou EXTENSIBLE).
struct SampleKind {
    bool     is_float = false;
    uint16_t bits     = 0;  // taille du conteneur (16, 24, 32)
    uint32_t mask     = 0;  // positions des canaux (EXTENSIBLE), 0 = inconnu
};

bool sample_kind(const WAVEFORMATEX* f, SampleKind& k) {
    if (!f) return false;
    k      = {};
    k.bits = f->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) k.is_float = true;
    else if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        k.mask        = x->dwChannelMask;
        if (IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) k.is_float = true;
        else if (!IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) return false;
    } else if (f->wFormatTag != WAVE_FORMAT_PCM) return false;
    if (k.is_float) return k.bits == 32;
    return k.bits == 16 || k.bits == 24 || k.bits == 32;
}

// Trames capturées -> float entrelacé (mêmes canaux).
void to_float(const BYTE* src, UINT32 frames, UINT32 channels, const SampleKind& k, float* dst) {
    const size_t n = size_t(frames) * channels;
    if (k.is_float) {
        std::memcpy(dst, src, n * sizeof(float));
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        if (k.bits == 16) {
            int16_t v;
            std::memcpy(&v, src + i * 2, 2);
            dst[i] = float(v) * (1.0f / 32768.0f);
        } else if (k.bits == 24) {
            const BYTE* p = src + i * 3;
            const int32_t v = int32_t(uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24) >> 8;
            dst[i]          = float(v) * (1.0f / 8388608.0f);
        } else {
            int32_t v;
            std::memcpy(&v, src + i * 4, 4);
            dst[i] = float(double(v) * (1.0 / 2147483648.0));
        }
    }
}

// Format de sortie : float 32 bits, même fréquence et mêmes canaux que l'entrée (le moteur audio convertit la
// fréquence vers son format de mixage, AUTOCONVERTPCM).
WAVEFORMATEXTENSIBLE float_format(UINT32 rate, UINT16 channels, UINT32 mask) {
    WAVEFORMATEXTENSIBLE x{};
    x.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels            = channels;
    x.Format.nSamplesPerSec       = rate;
    x.Format.wBitsPerSample       = 32;
    x.Format.nBlockAlign          = WORD(channels * 4);
    x.Format.nAvgBytesPerSec      = rate * x.Format.nBlockAlign;
    x.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    x.Samples.wValidBitsPerSample = 32;
    x.dwChannelMask               = mask ? mask : (channels == 1 ? SPEAKER_FRONT_CENTER : channels == 2 ? KSAUDIO_SPEAKER_STEREO : 0);
    x.SubFormat                   = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return x;
}

}  // namespace

std::vector<std::wstring> list_audio_inputs() {
    std::vector<std::wstring> names;
    for (auto& e : capture_endpoints()) names.push_back(e.name);
    return names;
}

struct AudioPassthrough::Impl {
    ComPtr<IAudioClient>        cap_client, out_client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient>  render;
    ComPtr<ISimpleAudioVolume>  volume;
    WAVEFORMATEX*               fmt = nullptr;  // format de capture (GetMixFormat de l'entrée)
    SampleKind                  kind;
    WAVEFORMATEXTENSIBLE        out_fmt{};      // format écrit en sortie (float)
    HANDLE                      event = nullptr;
    UINT32                      out_frames = 0;

    ~Impl() {
        if (fmt) CoTaskMemFree(fmt);
        if (event) CloseHandle(event);
    }
};

AudioPassthrough::AudioPassthrough() = default;
AudioPassthrough::~AudioPassthrough() { stop(); }

bool AudioPassthrough::start(const std::wstring& needle, const std::string& video_device_name, float vol) {
    stop();
    auto endpoints = capture_endpoints();
    const Endpoint* chosen = nullptr;
    if (!needle.empty()) {
        for (auto& e : endpoints)
            if (to_lower(e.name).find(to_lower(needle)) != std::wstring::npos) { chosen = &e; break; }
    } else {
        // 1) une entrée qui partage un mot avec le nom de la carte vidéo ; 2) un nom typique de carte.
        for (auto& word : words_of(widen(video_device_name))) {
            for (auto& e : endpoints)
                if (to_lower(e.name).find(word) != std::wstring::npos) { chosen = &e; break; }
            if (chosen) break;
        }
        static const wchar_t* kHints[] = {L"elgato", L"avermedia", L"capture", L"hdmi", L"cam link",
                                          L"live gamer", L"digital audio interface", L"usb video"};
        for (auto* hint : kHints) {
            if (chosen) break;
            for (auto& e : endpoints)
                if (to_lower(e.name).find(hint) != std::wstring::npos) { chosen = &e; break; }
        }
    }
    if (!chosen) {
        log_msg("Son : aucune entrée audio correspondant à la carte (utilise --audio-device, voir --list-devices).");
        return false;
    }

    impl_ = std::make_unique<Impl>();
    auto& m = *impl_;
    m.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    check_hr(chosen->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(m.cap_client.GetAddressOf())),
             "IAudioClient (capture)");
    check_hr(m.cap_client->GetMixFormat(&m.fmt), "GetMixFormat");
    if (!sample_kind(m.fmt, m.kind)) {
        log_msg("Son : format de l'entrée non géré (tag 0x%04X, %u bits)", unsigned(m.fmt->wFormatTag),
                unsigned(m.fmt->wBitsPerSample));
        impl_.reset();
        return false;
    }
    check_hr(m.cap_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, m.fmt,
                                      nullptr),
             "Initialize (capture)");
    check_hr(m.cap_client->SetEventHandle(m.event), "SetEventHandle");
    check_hr(m.cap_client->GetService(IID_PPV_ARGS(&m.capture)), "IAudioCaptureClient");

    ComPtr<IMMDeviceEnumerator> en;
    check_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)),
             "MMDeviceEnumerator");
    ComPtr<IMMDevice> out_dev;
    check_hr(en->GetDefaultAudioEndpoint(eRender, eConsole, &out_dev), "sortie audio par défaut");
    check_hr(out_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(m.out_client.GetAddressOf())),
             "IAudioClient (sortie)");
    m.out_fmt = float_format(m.fmt->nSamplesPerSec, m.fmt->nChannels, m.kind.mask);
    check_hr(m.out_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      600000, 0, &m.out_fmt.Format, nullptr),
             "Initialize (sortie)");
    check_hr(m.out_client->GetBufferSize(&m.out_frames), "GetBufferSize");
    check_hr(m.out_client->GetService(IID_PPV_ARGS(&m.render)), "IAudioRenderClient");
    if (SUCCEEDED(m.out_client->GetService(IID_PPV_ARGS(&m.volume)))) m.volume->SetMasterVolume(std::clamp(vol, 0.0f, 1.0f), nullptr);

    // Silence d'amorçage = remplissage visé du tampon de sortie (évite la famine des premières périodes).
    const UINT32 prime = std::min(m.out_frames, audio_delay::ms_to_frames(audio_sync::PumpConfig{}.out_fill_ms,
                                                                          m.out_fmt.Format.nSamplesPerSec));
    BYTE* dst = nullptr;
    if (SUCCEEDED(m.render->GetBuffer(prime, &dst))) m.render->ReleaseBuffer(prime, AUDCLNT_BUFFERFLAGS_SILENT);

    check_hr(m.out_client->Start(), "Start (sortie)");
    check_hr(m.cap_client->Start(), "Start (capture)");

    name_ = narrow(chosen->name);
    log_msg("Son : \"%s\" -> sortie par défaut (%lu Hz, %u canaux)", name_.c_str(), m.fmt->nSamplesPerSec,
            unsigned(m.fmt->nChannels));
    running_ = true;
    thread_  = std::thread([this] { loop(); });
    return true;
}

void AudioPassthrough::loop() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto&         m  = *impl_;
    // File intermédiaire + rééchantillonnage asservi (audio_sync.hpp) : les deux horloges (carte, sortie) ne
    // sont pas les mêmes ; la latence est tenue autour de ~25 ms + le retard voulu (set_delay_ms) sans jeter
    // de paquets en régime normal.
    audio_sync::PumpConfig cfg;
    cfg.in_rate  = m.fmt->nSamplesPerSec;
    cfg.out_rate = m.out_fmt.Format.nSamplesPerSec;
    cfg.channels = m.out_fmt.Format.nChannels;
    audio_sync::Pump   pump(cfg);
    const UINT32       ch = cfg.channels;
    std::vector<float> conv, out(size_t(m.out_frames) * ch);

    while (running_) {
        if (WaitForSingleObject(m.event, 100) != WAIT_OBJECT_0) continue;
        UINT32 packet = 0;
        while (running_ && SUCCEEDED(m.capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            if (FAILED(m.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pump.push(nullptr, frames);
            } else {
                conv.resize(size_t(frames) * ch);
                to_float(data, frames, ch, m.kind, conv.data());
                pump.push(conv.data(), frames);
            }
            m.capture->ReleaseBuffer(frames);
        }

        UINT32 padding = 0;
        if (FAILED(m.out_client->GetCurrentPadding(&padding))) continue;
        const UINT32 room = m.out_frames > padding ? m.out_frames - padding : 0;
        // Tampon de sortie réservé AVANT de tirer de la file : si GetBuffer échoue, rien n'est consommé ni compté.
        BYTE*        dst  = nullptr;
        if (!room || FAILED(m.render->GetBuffer(room, &dst))) continue;
        const UINT32 written = pump.pull(out.data(), room, padding, delay_ms_, now_seconds());
        std::memcpy(dst, out.data(), size_t(written) * ch * sizeof(float));
        m.render->ReleaseBuffer(written, 0);
        frames_           = pump.relayed();
        dropped_          = pump.dropped();
        drift_ppm_        = float(pump.ppm());
        buffered_ms_      = float(padding + written) * 1000.0f / float(cfg.out_rate);
        applied_delay_ms_ = float(pump.queued()) * 1000.0f / float(cfg.in_rate);
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

void AudioPassthrough::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (impl_) {
        if (impl_->cap_client) impl_->cap_client->Stop();
        if (impl_->out_client) impl_->out_client->Stop();
        impl_.reset();
    }
}

void AudioPassthrough::set_volume(float v) {
    if (impl_ && impl_->volume) impl_->volume->SetMasterVolume(std::clamp(v, 0.0f, 1.0f), nullptr);
}

std::string AudioPassthrough::name() const { return name_; }
