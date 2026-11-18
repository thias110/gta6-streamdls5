#include "audio_passthrough.hpp"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>
// Définit PKEY_Device_FriendlyName dans cette unité (pas de .lib qui la fournit).
#include <initguid.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cwctype>

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
    WAVEFORMATEX*               fmt = nullptr;
    HANDLE                      event = nullptr;
    UINT32                      out_frames = 0;
    std::mutex                  mu;

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
    check_hr(m.out_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      600000, 0, m.fmt, nullptr),
             "Initialize (sortie)");
    check_hr(m.out_client->GetBufferSize(&m.out_frames), "GetBufferSize");
    check_hr(m.out_client->GetService(IID_PPV_ARGS(&m.render)), "IAudioRenderClient");
    if (SUCCEEDED(m.out_client->GetService(IID_PPV_ARGS(&m.volume)))) m.volume->SetMasterVolume(std::clamp(vol, 0.0f, 1.0f), nullptr);

    // 10 ms de silence pour amorcer la sortie.
    const UINT32 prime = m.fmt->nSamplesPerSec / 100;
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
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto&        m          = *impl_;
    const UINT32 block      = m.fmt->nBlockAlign;
    const UINT32 rate       = m.fmt->nSamplesPerSec;
    const UINT32 max_queued = rate * 60 / 1000;  // au-delà de 60 ms en tampon, on jette

    while (running_) {
        if (WaitForSingleObject(m.event, 100) != WAIT_OBJECT_0) continue;
        UINT32 packet = 0;
        while (running_ && SUCCEEDED(m.capture->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            if (FAILED(m.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;

            UINT32 padding = 0;
            m.out_client->GetCurrentPadding(&padding);
            const UINT32 room  = m.out_frames - padding;
            UINT32       write = std::min(frames, room);
            if (padding > max_queued) write = 0;  // la sortie a pris du retard : on rattrape
            if (write > 0) {
                BYTE* dst = nullptr;
                if (SUCCEEDED(m.render->GetBuffer(write, &dst))) {
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::memset(dst, 0, size_t(write) * block);
                    else std::memcpy(dst, data, size_t(write) * block);
                    m.render->ReleaseBuffer(write, 0);
                }
            }
            frames_ += write;
            dropped_ += frames - write;
            buffered_ms_ = float(padding + write) * 1000.0f / float(rate);
            m.capture->ReleaseBuffer(frames);
        }
    }
    CoUninitialize();
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
