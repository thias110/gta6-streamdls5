#include "audio_passthrough.hpp"
#include "audio_delay.hpp"
#include "audio_format.hpp"
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

namespace af = audio_format;

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

// WAVEFORMATEX (PCM, IEEE_FLOAT ou EXTENSIBLE) -> description pure ; sample = Unknown si non géré.
af::Pcm pcm_of(const WAVEFORMATEX* f) {
    af::Pcm p;
    if (!f) return p;
    p.rate        = f->nSamplesPerSec;
    p.channels    = f->nChannels;
    bool is_float = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool known    = is_float || f->wFormatTag == WAVE_FORMAT_PCM;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= 22) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        p.mask        = x->dwChannelMask;
        is_float      = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
        known         = is_float || IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }
    if (!known || f->nBlockAlign != f->nChannels * (f->wBitsPerSample / 8)) return p;
    if (is_float) p.sample = f->wBitsPerSample == 32 ? af::Sample::Float32 : af::Sample::Unknown;
    else if (f->wBitsPerSample == 16) p.sample = af::Sample::Int16;
    else if (f->wBitsPerSample == 24) p.sample = af::Sample::Int24;
    else if (f->wBitsPerSample == 32) p.sample = af::Sample::Int32;  // y compris 24 bits utiles dans 32
    return p;
}

WAVEFORMATEXTENSIBLE wave_of(const af::Pcm& p) {
    const bool           is_float = p.sample == af::Sample::Float32;
    const WORD           bits     = WORD(af::sample_bytes(p.sample) * 8);
    WAVEFORMATEXTENSIBLE x{};
    x.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels            = p.channels;
    x.Format.nSamplesPerSec       = p.rate;
    x.Format.wBitsPerSample       = bits;
    x.Format.nBlockAlign          = WORD(p.block());
    x.Format.nAvgBytesPerSec      = p.rate * p.block();
    x.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    x.Samples.wValidBitsPerSample = bits;
    x.dwChannelMask               = p.mask ? p.mask : af::default_mask(p.channels);
    x.SubFormat                   = is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
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
    af::Pcm                     in;             // le même, décrit
    af::Pcm                     out;            // format écrit en sortie (négocié, voir audio_format.hpp)
    std::vector<float>          matrix;         // mixage des canaux entrée -> sortie
    HANDLE                      event = nullptr;
    UINT32                      out_frames = 0;

    ~Impl() {
        if (cap_client) cap_client->Stop();
        if (out_client) out_client->Stop();
        if (fmt) CoTaskMemFree(fmt);
        if (event) CloseHandle(event);
    }
};

AudioPassthrough::AudioPassthrough() = default;
AudioPassthrough::~AudioPassthrough() { stop(); }

bool AudioPassthrough::start(const std::wstring& needle, const std::string& video_device_name, float vol) {
    stop();
    needle_     = needle;
    video_name_ = video_device_name;
    volume_     = std::clamp(vol, 0.0f, 1.0f);
    if (!open(true)) return false;
    name_    = impl_name_;
    running_ = true;
    thread_  = std::thread([this] { loop(); });
    return true;
}

// Choisit l'entrée, négocie les formats et démarre les deux flux (impl_). false si aucune entrée ne
// correspond ; lève en cas d'échec WASAPI. Appelée par start() puis par le thread du relais (réouverture).
bool AudioPassthrough::open(bool verbose) {
    impl_.reset();
    const std::wstring& needle = needle_;
    auto endpoints = capture_endpoints();
    const Endpoint* chosen = nullptr;
    if (!needle.empty()) {
        for (auto& e : endpoints)
            if (to_lower(e.name).find(to_lower(needle)) != std::wstring::npos) { chosen = &e; break; }
    } else {
        const std::string& video_device_name = video_name_;
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
        if (verbose) log_msg("Son : aucune entrée audio correspondant à la carte (utilise --audio-device, voir --list-devices).");
        return false;
    }

    impl_ = std::make_unique<Impl>();
    auto& m = *impl_;
    m.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    check_hr(chosen->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(m.cap_client.GetAddressOf())),
             "IAudioClient (capture)");
    check_hr(m.cap_client->GetMixFormat(&m.fmt), "GetMixFormat");
    m.in = pcm_of(m.fmt);
    if (!m.in.valid()) {
        log_msg("Son : format de l'entrée non géré (tag 0x%04X, %u bits, %u canaux)", unsigned(m.fmt->wFormatTag),
                unsigned(m.fmt->wBitsPerSample), unsigned(m.fmt->nChannels));
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
    // Format de sortie : on n'impose plus le format de l'entrée (un 5.1/7.1 peut être refusé) ; on essaie des
    // formats simples puis le format de mixage exact de la sortie (audio_format::output_candidates).
    af::Pcm mix;
    {
        ComPtr<IAudioClient> probe;
        WAVEFORMATEX*        mf = nullptr;
        if (SUCCEEDED(out_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(probe.GetAddressOf()))) &&
            SUCCEEDED(probe->GetMixFormat(&mf))) {
            mix = pcm_of(mf);
            if (!mix.valid()) mix.sample = af::Sample::Unknown;
        }
        if (mf) CoTaskMemFree(mf);
    }
    HRESULT last = E_FAIL;
    for (const auto& c : af::output_candidates(m.in, mix)) {
        ComPtr<IAudioClient> client;  // un IAudioClient par essai : Initialize ne se rappelle pas après un échec
        last = out_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(last)) break;
        const WAVEFORMATEXTENSIBLE w     = wave_of(c.fmt);
        const DWORD                flags = c.autoconvert
                                               ? AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
                                               : 0;
        last = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 600000, 0, &w.Format, nullptr);
        if (SUCCEEDED(last)) {
            m.out_client = client;
            m.out        = c.fmt;
            break;
        }
        log_msg("Son : sortie %s refusée (0x%08lX)", af::describe(c.fmt).c_str(), static_cast<unsigned long>(last));
    }
    check_hr(m.out_client ? S_OK : last, "Initialize (sortie) : aucun format accepté");
    m.matrix = af::mix_matrix(m.in.channels, m.in.mask, m.out.channels);
    check_hr(m.out_client->GetBufferSize(&m.out_frames), "GetBufferSize");
    check_hr(m.out_client->GetService(IID_PPV_ARGS(&m.render)), "IAudioRenderClient");
    if (SUCCEEDED(m.out_client->GetService(IID_PPV_ARGS(&m.volume)))) m.volume->SetMasterVolume(volume_, nullptr);
    volume_dirty_ = false;

    // Silence d'amorçage = remplissage visé du tampon de sortie (évite la famine des premières périodes).
    const UINT32 prime = std::min(m.out_frames, audio_delay::ms_to_frames(audio_sync::PumpConfig{}.out_fill_ms,
                                                                          m.out.rate));
    BYTE* dst = nullptr;
    if (SUCCEEDED(m.render->GetBuffer(prime, &dst))) m.render->ReleaseBuffer(prime, AUDCLNT_BUFFERFLAGS_SILENT);

    check_hr(m.out_client->Start(), "Start (sortie)");
    check_hr(m.cap_client->Start(), "Start (capture)");

    impl_name_ = narrow(chosen->name);
    log_msg("Son : \"%s\" (%s) -> sortie par défaut (%s)", impl_name_.c_str(), af::describe(m.in).c_str(),
            af::describe(m.out).c_str());
    return true;
}

// Relais tant que tout va bien ; renvoie le HRESULT de la première erreur (périphérique débranché ou invalidé :
// AUDCLNT_E_DEVICE_INVALIDATED, changement de sortie par défaut...), S_OK si arrêt demandé.
HRESULT AudioPassthrough::relay() {
    auto& m = *impl_;
    // File intermédiaire + rééchantillonnage asservi (audio_sync.hpp) : les deux horloges (carte, sortie) ne
    // sont pas les mêmes ; la latence est tenue autour de ~25 ms + le retard voulu (set_delay_ms) sans jeter
    // de paquets en régime normal.
    audio_sync::PumpConfig cfg;
    cfg.in_rate  = m.in.rate;
    cfg.out_rate = m.out.rate;  // diffère de l'entrée seulement avec le format de mixage exact de la sortie
    cfg.channels = m.out.channels;
    audio_sync::Pump   pump(cfg);
    const UINT32       ch = cfg.channels;
    std::vector<float> conv, scratch, out(size_t(m.out_frames) * ch);
    const uint64_t     base_frames = frames_, base_dropped = dropped_;
    int                idle        = 0;  // attentes sans paquet (100 ms chacune)

    while (running_) {
        if (volume_dirty_.exchange(false) && m.volume) m.volume->SetMasterVolume(volume_, nullptr);
        if (WaitForSingleObject(m.event, 100) != WAIT_OBJECT_0) {
            // Plus d'événement : entrée silencieuse ou périphérique mort ; on sonde toutes les ~2 s.
            UINT32 probe = 0;
            if (++idle >= 20) {
                idle = 0;
                UINT32 pad = 0;
                if (const HRESULT hr = m.capture->GetNextPacketSize(&probe); FAILED(hr)) return hr;
                if (const HRESULT hr = m.out_client->GetCurrentPadding(&pad); FAILED(hr)) return hr;
            }
            continue;
        }
        idle = 0;
        for (;;) {
            UINT32  packet = 0;
            HRESULT hr     = m.capture->GetNextPacketSize(&packet);
            if (FAILED(hr)) return hr;
            if (!packet || !running_) break;
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            hr            = m.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) return hr;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pump.push(nullptr, frames);
            } else {
                conv.resize(size_t(frames) * ch);
                af::to_float(data, frames, m.in, m.out.channels, m.matrix, conv.data(), scratch);
                pump.push(conv.data(), frames);
            }
            if (FAILED(hr = m.capture->ReleaseBuffer(frames))) return hr;
        }

        UINT32 padding = 0;
        if (const HRESULT hr = m.out_client->GetCurrentPadding(&padding); FAILED(hr)) return hr;
        const UINT32 room = m.out_frames > padding ? m.out_frames - padding : 0;
        if (!room) continue;
        // Tampon de sortie réservé AVANT de tirer de la file : si GetBuffer échoue, rien n'est consommé ni compté.
        BYTE* dst = nullptr;
        if (const HRESULT hr = m.render->GetBuffer(room, &dst); FAILED(hr)) return hr;
        const UINT32 written = pump.pull(out.data(), room, padding, delay_ms_, now_seconds());
        af::from_float(out.data(), written, m.out, dst);
        if (const HRESULT hr = m.render->ReleaseBuffer(written, 0); FAILED(hr)) return hr;

        frames_           = base_frames + pump.relayed();
        dropped_          = base_dropped + pump.dropped();
        drift_ppm_        = float(pump.ppm());
        buffered_ms_      = float(padding + written) * 1000.0f / float(cfg.out_rate);
        applied_delay_ms_ = float(pump.queued()) * 1000.0f / float(cfg.in_rate);
    }
    return S_OK;
}

void AudioPassthrough::loop() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Attente interruptible par stop().
    auto sleep_s = [this](double s) {
        for (double t = 0.0; running_ && t < s; t += 0.05) Sleep(50);
    };
    int attempt = 0;
    while (running_) {
        if (!impl_) {
            sleep_s(audio_sync::retry_delay_s(attempt));
            if (!running_) break;
            std::string why;
            try {
                if (open(false)) {
                    log_msg("Son : relais rétabli après %d tentative(s)", attempt + 1);
                    attempt = 0;
                    continue;
                }
                why = "entrée introuvable";
            } catch (const std::exception& e) {
                why = e.what();
            }
            impl_.reset();
            if (++attempt >= audio_sync::kMaxRetries) {
                log_msg("Son : relais abandonné après %d tentatives (%s) ; l'image continue sans le son.", attempt,
                        why.c_str());
                running_ = false;
                break;
            }
            if (attempt == 1) log_msg("Son : réouverture impossible (%s) ; nouvelles tentatives en arrière-plan.", why.c_str());
            continue;
        }
        const HRESULT hr = relay();
        if (!running_) break;
        // Erreur journalisée une fois par incident ; flux fermés ; réouverture avec temporisation.
        log_msg("Son : périphérique audio perdu ou en erreur (0x%08lX%s) ; tentative de réouverture.",
                static_cast<unsigned long>(hr), hr == AUDCLNT_E_DEVICE_INVALIDATED ? ", AUDCLNT_E_DEVICE_INVALIDATED" : "");
        impl_.reset();
        buffered_ms_ = 0.0f;
        attempt      = 0;
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

void AudioPassthrough::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    impl_.reset();  // ~Impl arrête les flux
}

void AudioPassthrough::set_volume(float v) {
    volume_       = std::clamp(v, 0.0f, 1.0f);
    volume_dirty_ = true;  // appliqué par le thread du relais (seul à toucher aux objets WASAPI)
}

std::string AudioPassthrough::name() const { return name_; }
