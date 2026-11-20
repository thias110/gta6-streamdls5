#include "mf_capture.hpp"
#include "capture_logic.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <cmath>
#include <cwchar>
#include <mutex>
#include <optional>
#include <thread>

namespace cl = capture_logic;

namespace {

// MFSampleExtension_DeviceReferenceSystemTime (déclaré seulement par les SDK récents, d'où la copie) : instant
// de capture sur l'horloge QPC du système, en 100 ns. Même base que now_seconds().
const GUID kDeviceReferenceSystemTime = {0x6523775a, 0xba2d, 0x405f, {0xb2, 0xc5, 0x01, 0xff, 0x88, 0xe2, 0xe8, 0xf6}};

// MFStartup/MFShutdown comptés en références.
struct MfRuntime {
    MfRuntime() { check_hr(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET), "MFStartup"); }
    ~MfRuntime() { MFShutdown(); }
    MfRuntime(const MfRuntime&)            = delete;
    MfRuntime& operator=(const MfRuntime&) = delete;
};

struct Device {
    ComPtr<IMFActivate> activate;
    std::wstring        name;
};

std::vector<Device> enum_devices() {
    ComPtr<IMFAttributes> attr;
    check_hr(MFCreateAttributes(&attr, 1), "MFCreateAttributes");
    check_hr(attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
             "SetGUID VIDCAP");
    IMFActivate** arr = nullptr;
    UINT32        n   = 0;
    check_hr(MFEnumDeviceSources(attr.Get(), &arr, &n), "MFEnumDeviceSources");
    // Toutes les références sont prises d'abord (et le tableau libéré) : rien ne fuit si la suite échoue.
    std::vector<Device> out(n);
    for (UINT32 i = 0; i < n; ++i) out[i].activate.Attach(arr[i]);
    CoTaskMemFree(arr);
    for (auto& d : out) {
        WCHAR* name = nullptr;
        UINT32 len  = 0;
        if (SUCCEEDED(d.activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
            d.name.assign(name, len);
            CoTaskMemFree(name);
        }
    }
    return out;
}

std::vector<std::wstring> names_of(const std::vector<Device>& devices) {
    std::vector<std::wstring> names;
    for (const auto& d : devices) names.push_back(d.name);
    return names;
}

cl::Subtype to_subtype(const GUID& g) {
    if (g == MFVideoFormat_NV12) return cl::Subtype::NV12;
    if (g == MFVideoFormat_YUY2) return cl::Subtype::YUY2;
    if (g == MFVideoFormat_MJPG) return cl::Subtype::MJPG;
    if (g == MFVideoFormat_RGB32) return cl::Subtype::RGB32;
    if (g == MFVideoFormat_P010) return cl::Subtype::P010;
    if (g == MFVideoFormat_H264) return cl::Subtype::H264;
    if (g == MFVideoFormat_HEVC) return cl::Subtype::HEVC;
    return cl::Subtype::Other;
}

// Modes natifs du premier flux vidéo (types gardés dans le même ordre si `types` est fourni).
std::vector<cl::NativeMode> native_modes(IMFSourceReader* reader, std::vector<ComPtr<IMFMediaType>>* types) {
    std::vector<cl::NativeMode> modes;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        const HRESULT hr = reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &t);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
        GUID   sub{};
        UINT32 w = 0, h = 0, num = 0, den = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        modes.push_back({w, h, den ? double(num) / double(den) : 0.0, to_subtype(sub)});
        if (types) types->push_back(t);
    }
    return modes;
}

// Lecture asynchrone (MF_SOURCE_READER_ASYNC_CALLBACK) : ReadSample ne bloque jamais ; chaque échantillon
// arrive sur un thread de travail MF et la demande suivante part de là. Le propriétaire est détaché sous
// verrou à l'arrêt : après detach(), aucun rappel ne touche plus à la source (même s'il arrive en retard).
class ReaderCallback final : public IMFSourceReaderCallback {
public:
    // Renvoie true pour demander l'échantillon suivant.
    using Sink = bool (*)(void* owner, HRESULT hr, DWORD flags, LONGLONG ts, IMFSample* sample);

    ReaderCallback(void* owner, Sink sink) : owner_(owner), sink_(sink) {
        flushed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ReaderCallback(const ReaderCallback&)            = delete;
    ReaderCallback& operator=(const ReaderCallback&) = delete;

    void set_reader(IMFSourceReader* r) {
        std::lock_guard lk(mu_);
        reader_ = r;  // pas de référence : le reader possède le rappel (pas de cycle) ; effacé par detach()
    }
    void detach() {
        std::lock_guard lk(mu_);  // attend la fin d'un rappel en cours (court : aucun appel bloquant dedans)
        owner_  = nullptr;
        reader_ = nullptr;
    }
    HANDLE flushed() const { return flushed_; }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
            *out = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ULONG(++refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const long n = --refs_;
        if (n == 0) delete this;
        return ULONG(n);
    }
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD /*stream*/, DWORD flags, LONGLONG ts, IMFSample* sample) override {
        std::lock_guard lk(mu_);  // récursif : un ReadSample qui rappellerait sur le même thread ne bloque pas
        if (!owner_ || !reader_) return S_OK;
        if (sink_(owner_, hr, flags, ts, sample)) {
            const HRESULT next = reader_->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr,
                                                     nullptr, nullptr);
            if (FAILED(next)) sink_(owner_, next, MF_SOURCE_READERF_ERROR, 0, nullptr);
        }
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override {
        SetEvent(flushed_);
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    ~ReaderCallback() {
        if (flushed_) CloseHandle(flushed_);
    }
    std::atomic<long>    refs_{1};
    std::recursive_mutex mu_;
    void*                owner_  = nullptr;
    Sink                 sink_   = nullptr;
    IMFSourceReader*     reader_ = nullptr;
    HANDLE               flushed_ = nullptr;
};

}  // namespace

std::vector<std::wstring> list_capture_devices() {
    MfRuntime rt;
    return names_of(enum_devices());
}

bool list_capture_modes(const std::wstring& needle, std::wstring& device, std::vector<std::string>& modes) {
    MfRuntime rt;
    auto      devices = enum_devices();
    const int idx     = cl::pick_device(names_of(devices), needle);
    if (idx < 0) return false;
    Device& d = devices[size_t(idx)];
    device    = d.name;
    ComPtr<IMFMediaSource> source;
    check_hr(d.activate->ActivateObject(IID_PPV_ARGS(&source)), "ActivateObject (carte de capture)");
    ComPtr<IMFSourceReader> reader;
    const HRESULT hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
    if (SUCCEEDED(hr))
        for (const auto& m : native_modes(reader.Get(), nullptr)) modes.push_back(cl::describe(m));
    reader.Reset();
    source->Shutdown();
    d.activate->ShutdownObject();
    check_hr(hr, "MFCreateSourceReader");
    return true;
}

// ---------------------------------------------------------------------------------------------

struct MfCaptureSource::Impl {
    explicit Impl(D3DContext& d) : d3d(d) {}

    D3DContext&                 d3d;
    std::unique_ptr<MfRuntime>  runtime;
    ComPtr<IMFDXGIDeviceManager> devmgr;
    ComPtr<IMFMediaSource>      source;
    ComPtr<IMFSourceReader>     reader;
    cl::Output                  output = cl::Output::NV12;  // format demandé au Source Reader (fixé au démarrage)

    std::thread       thread;            // lecture synchrone (--capture-sync) uniquement
    ComPtr<ReaderCallback> callback;     // lecture asynchrone (défaut)
    bool              sync_read = false;
    bool              discontinuity = true;  // prochain échantillon : recalage de l'horodatage
    std::atomic<bool> running{false};
    std::atomic<bool> failed{false};
    std::atomic<int>  basis{int(cl::TimestampMapper::Basis::None)};
    HANDLE            event = nullptr;

    cl::TimestampMapper mapper;        // thread de lecture uniquement

    std::mutex        mu;              // protège latest*, width, height, colour, stride
    ComPtr<IMFSample> latest;
    double            latest_time = 0.0;
    UINT32            width = 0, height = 0;
    bool              full_range = false, bt601 = false;
    LONG              stride = 0;      // MF_MT_DEFAULT_STRIDE (RGB32 en mémoire système ; < 0 = bas en haut)
    std::string       label;

    // Conversion -> BGRA par le VideoProcessor (thread de rendu uniquement)
    struct InView {
        ComPtr<ID3D11Texture2D>               tex;
        UINT                                  slice = 0;
        ComPtr<ID3D11VideoProcessorInputView> view;
    };
    ComPtr<ID3D11VideoProcessorEnumerator> en;
    ComPtr<ID3D11VideoProcessor>           vp;
    ComPtr<ID3D11VideoProcessorOutputView> out_view;
    std::vector<InView>                    views;
    UINT32                                 vp_w = 0, vp_h = 0;
    bool                                   vp_full = false, vp_601 = false;
    GpuTexture                             out;
    // Chemin mémoire système : deux textures staging en alternance (Map n'attend pas la copie GPU précédente).
    ComPtr<ID3D11Texture2D>                staging[2], upload;
    int                                    staging_i = 0;
    std::string                            error;  // dernière erreur fatale (thread de rendu)
    bool                                   logged_path  = false;
    int                                    logged_basis = -1;

    void read_format();
    void loop();
    // Traite le résultat d'une lecture (synchrone ou asynchrone) ; false = arrêter de lire.
    bool on_sample(HRESULT hr, DWORD flags, LONGLONG ts, IMFSample* sample);
    bool ensure_processor(UINT32 w, UINT32 h, bool full, bool b601);
    ID3D11VideoProcessorInputView* input_view(ID3D11Texture2D* tex, UINT slice);
    bool upload_system_memory(IMFMediaBuffer* buf, UINT32 w, UINT32 h, LONG default_stride);
};

void MfCaptureSource::Impl::read_format() {
    ComPtr<IMFMediaType> cur;
    check_hr(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur), "GetCurrentMediaType");
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    const UINT32 range  = MFGetAttributeUINT32(cur.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    // Matrice absente du type média : BT.601 en SD (< 720 lignes), BT.709 au-delà (même règle que le frame tap).
    const UINT32 matrix = MFGetAttributeUINT32(cur.Get(), MF_MT_YUV_MATRIX,
                                               h < 720 ? MFVideoTransferMatrix_BT601 : MFVideoTransferMatrix_BT709);
    const LONG   st     = LONG(MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, 0));  // UINT32 signé
    std::lock_guard lk(mu);
    width      = w;
    height     = h;
    full_range = range == MFNominalRange_0_255;
    bt601      = matrix == MFVideoTransferMatrix_BT601;
    stride     = st;
}

bool MfCaptureSource::Impl::on_sample(HRESULT hr, DWORD flags, LONGLONG ts, IMFSample* sample_in) {
    if (!running) return false;
    if (FAILED(hr)) {
        log_msg("Carte de capture : ReadSample en échec (0x%08lX)", static_cast<unsigned long>(hr));
        failed = true;
        SetEvent(event);  // réveille le thread de rendu : alive() est devenu faux
        return false;
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
        log_msg("Carte de capture : fin du flux (débranchée ?)");
        failed = true;
        SetEvent(event);
        return false;
    }
    if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
        try {
            read_format();
        } catch (const std::exception& e) {
            log_msg("Carte de capture : nouveau format illisible (%s)", e.what());
            failed = true;
            SetEvent(event);
            return false;
        }
        UINT32 w = 0, h = 0;
        {
            std::lock_guard lk(mu);
            w = width;
            h = height;
        }
        log_msg("Carte de capture : nouveau format %ux%u", w, h);
        discontinuity = true;
    }
    if (flags & MF_SOURCE_READERF_STREAMTICK) discontinuity = true;  // trou dans le flux
    if (!sample_in) return true;
    ComPtr<IMFSample> sample(sample_in);

    const double           arrival = now_seconds();
    std::optional<int64_t> device_time;
    UINT64                 dt = 0;
    if (SUCCEEDED(sample->GetUINT64(kDeviceReferenceSystemTime, &dt)) && dt) device_time = int64_t(dt);
    if (MFGetAttributeUINT32(sample.Get(), MFSampleExtension_Discontinuity, FALSE)) discontinuity = true;
    // Horodatage de l'échantillon seulement s'il en a un (GetSampleTime échoue sinon) ; `ts` de ReadSample en repli.
    std::optional<int64_t> sample_time;
    LONGLONG               st = 0;
    if (SUCCEEDED(sample->GetSampleTime(&st))) sample_time = int64_t(st);
    else if (ts > 0) sample_time = int64_t(ts);
    const double t = mapper.map(device_time, sample_time, discontinuity, arrival);
    discontinuity  = false;
    basis          = int(mapper.basis());
    {
        std::lock_guard lk(mu);
        latest      = sample;  // la précédente, jamais lue, est abandonnée : seule la plus récente compte
        latest_time = t;
    }
    SetEvent(event);
    return true;
}

void MfCaptureSource::Impl::loop() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running) {
        DWORD             stream = 0, flags = 0;
        LONGLONG          ts = 0;
        ComPtr<IMFSample> sample;
        // Bloquant (mode synchrone, --capture-sync), sur ce thread dédié ; Shutdown() de la source le débloque.
        const HRESULT hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &sample);
        if (!running) break;
        if (!on_sample(hr, flags, ts, sample.Get())) break;
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

bool MfCaptureSource::Impl::ensure_processor(UINT32 w, UINT32 h, bool full, bool b601) {
    if (vp && vp_w == w && vp_h == h && vp_full == full && vp_601 == b601) return true;
    auto* vdev = d3d.video_device();
    auto* vctx = d3d.video_ctx();
    if (!vdev || !vctx) throw std::runtime_error("API vidéo D3D11 indisponible : conversion en BGRA impossible");

    views.clear();
    out_view.Reset();
    vp.Reset();
    en.Reset();

    // Le même VideoProcessor sert pour une entrée NV12 (YUV -> RGB) ou RGB32 (copie, alpha opaque).
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate   = {60, 1};
    cd.InputWidth       = w;
    cd.InputHeight      = h;
    cd.OutputFrameRate  = {60, 1};
    cd.OutputWidth      = w;
    cd.OutputHeight     = h;
    cd.Usage            = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    check_hr(vdev->CreateVideoProcessorEnumerator(&cd, &en), "CreateVideoProcessorEnumerator (capture)");
    check_hr(vdev->CreateVideoProcessor(en.Get(), 0, &vp), "CreateVideoProcessor (capture)");

    out.ensure(d3d.device(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
               "capture BGRA");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    check_hr(vdev->CreateVideoProcessorOutputView(out.tex.Get(), en.Get(), &ovd, &out_view), "output view (capture)");

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.RGB_Range     = 0;  // entrée RGB32 : pleine plage
    in_cs.YCbCr_Matrix  = b601 ? 0 : 1;
    in_cs.Nominal_Range = full ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
    rgb.RGB_Range = 0;
    const RECT r{0, 0, LONG(w), LONG(h)};
    vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
    vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &in_cs);
    vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &rgb);
    vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &r);
    vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &r);
    vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &r);

    vp_w    = w;
    vp_h    = h;
    vp_full = full;
    vp_601  = b601;
    if (output == cl::Output::NV12)
        log_msg("Carte de capture : conversion NV12 %ux%u (BT.%s, plage %s) -> BGRA", w, h, b601 ? "601" : "709",
                full ? "pleine" : "limitée");
    else
        log_msg("Carte de capture : RGB32 %ux%u -> BGRA", w, h);
    return true;
}

ID3D11VideoProcessorInputView* MfCaptureSource::Impl::input_view(ID3D11Texture2D* tex, UINT slice) {
    for (auto& v : views)
        if (v.tex.Get() == tex && v.slice == slice) return v.view.Get();
    if (views.size() > 48) views.clear();  // le décodeur tourne sur un petit pool de textures
    InView iv;
    iv.tex   = tex;
    iv.slice = slice;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension          = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.ArraySlice   = slice;
    check_hr(d3d.video_device()->CreateVideoProcessorInputView(tex, en.Get(), &ivd, &iv.view), "input view (capture)");
    views.push_back(iv);
    return views.back().view.Get();
}

bool MfCaptureSource::Impl::upload_system_memory(IMFMediaBuffer* buf, UINT32 w, UINT32 h, LONG default_stride) {
    auto*             dev  = d3d.device();
    auto*             ctx  = d3d.ctx();
    const bool        nv12 = output == cl::Output::NV12;
    const DXGI_FORMAT fmt  = nv12 ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
    const size_t      row  = nv12 ? size_t(w) : size_t(w) * 4;  // octets utiles par ligne (plan Y pour le NV12)

    D3D11_TEXTURE2D_DESC have{};
    if (staging[0]) staging[0]->GetDesc(&have);
    if (!staging[0] || have.Width != w || have.Height != h || have.Format != fmt) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = fmt;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_STAGING;
        d.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
        staging[0].Reset();
        staging[1].Reset();
        upload.Reset();
        check_hr(dev->CreateTexture2D(&d, nullptr, &staging[0]), "staging (capture)");
        check_hr(dev->CreateTexture2D(&d, nullptr, &staging[1]), "staging (capture)");
        d.Usage          = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        d.BindFlags      = D3D11_BIND_RENDER_TARGET;
        check_hr(dev->CreateTexture2D(&d, nullptr, &upload), "upload (capture)");
    }

    // scan0 : première ligne affichée ; step : pas entre deux lignes (négatif pour une image de bas en haut).
    BYTE*               scan0 = nullptr;
    ptrdiff_t           step  = 0;
    ComPtr<IMF2DBuffer> b2;
    const bool          two_d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2)));
    if (two_d) {
        LONG pitch = 0;
        if (FAILED(b2->Lock2D(&scan0, &pitch))) return false;
        step = pitch;
    } else {
        BYTE* base = nullptr;
        DWORD len  = 0;
        if (FAILED(buf->Lock(&base, nullptr, &len))) return false;
        step = nv12 ? ptrdiff_t(w) : (default_stride ? ptrdiff_t(default_stride) : ptrdiff_t(row));
        const size_t abs_step = size_t(step < 0 ? -step : step);
        const size_t need     = nv12 ? abs_step * h * 3 / 2 : abs_step * h;
        if (abs_step < row || size_t(len) < need) {  // tampon plus court qu'une image : lecture hors limites
            buf->Unlock();
            return false;
        }
        scan0 = step < 0 ? base + abs_step * (h - 1) : base;
    }
    auto unlock = [&] {
        if (two_d) b2->Unlock2D();
        else buf->Unlock();
    };
    // NV12 : plans Y puis UV contigus, donc pas positif obligatoire ; RGB32 : |pas| >= une ligne.
    if ((nv12 && step < ptrdiff_t(w)) || size_t(step < 0 ? -step : step) < row) {
        unlock();
        return false;
    }

    ID3D11Texture2D* stg = staging[staging_i].Get();
    staging_i ^= 1;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(stg, 0, D3D11_MAP_WRITE, 0, &m);
    if (SUCCEEDED(hr)) {
        auto* dst = static_cast<BYTE*>(m.pData);
        for (UINT32 y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * m.RowPitch, scan0 + ptrdiff_t(y) * step, row);
        if (nv12) {
            const BYTE* uv_src = scan0 + step * ptrdiff_t(h);
            BYTE*       uv_dst = dst + size_t(m.RowPitch) * h;
            for (UINT32 y = 0; y < h / 2; ++y)
                std::memcpy(uv_dst + size_t(y) * m.RowPitch, uv_src + ptrdiff_t(y) * step, row);
        }
        ctx->Unmap(stg, 0);
        ctx->CopyResource(upload.Get(), stg);
    }
    unlock();
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------------------------

MfCaptureSource::MfCaptureSource(D3DContext& d3d) : impl_(std::make_unique<Impl>(d3d)) {
    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset, signalé à chaque image
}

MfCaptureSource::~MfCaptureSource() {
    stop();
    if (impl_->event) CloseHandle(impl_->event);
}

bool MfCaptureSource::start(const std::wstring& needle, const std::wstring& mode, bool sync_read) {
    impl_->sync_read = sync_read;
    // Échec (pilote, mode refusé, format non convertible...) : journalisé, ressources libérées, false ; l'appli
    // affiche alors la liste des périphériques au lieu d'un crash.
    try {
        if (start_impl(needle, mode)) return true;
    } catch (const std::exception& e) {
        log_msg("Carte de capture : démarrage impossible (%s)", e.what());
    }
    stop();
    return false;
}

bool MfCaptureSource::start_impl(const std::wstring& needle, const std::wstring& mode) {
    auto& m = *impl_;
    if (!m.d3d.video_device() || !m.d3d.video_ctx()) {
        log_msg("Carte de capture : API vidéo D3D11 indisponible sur ce GPU/pilote (nécessaire à la conversion en BGRA).");
        return false;
    }
    cl::ModeRequest req;
    std::string     err;
    if (!cl::parse_mode(narrow(mode), req, &err)) {
        log_msg("Carte de capture : %s", err.c_str());
        return false;
    }

    m.runtime    = std::make_unique<MfRuntime>();
    auto devices = enum_devices();
    if (devices.empty()) {
        log_msg("Aucune carte de capture / périphérique vidéo détecté.");
        return false;
    }
    const int idx = cl::pick_device(names_of(devices), needle);
    if (idx < 0) {
        log_msg("Aucun périphérique ne contient \"%s\".", narrow(needle).c_str());
        return false;
    }
    const Device& chosen = devices[size_t(idx)];

    check_hr(chosen.activate->ActivateObject(IID_PPV_ARGS(&m.source)), "ActivateObject (carte de capture)");

    UINT token = 0;
    check_hr(MFCreateDXGIDeviceManager(&token, &m.devmgr), "MFCreateDXGIDeviceManager");
    check_hr(m.devmgr->ResetDevice(m.d3d.device(), token), "IMFDXGIDeviceManager::ResetDevice");

    ComPtr<IMFAttributes> ra;
    check_hr(MFCreateAttributes(&ra, 4), "MFCreateAttributes reader");
    ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m.devmgr.Get());
    ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ra->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (!m.sync_read) {
        m.callback.Attach(new ReaderCallback(&m, [](void* o, HRESULT hr, DWORD flags, LONGLONG ts, IMFSample* smp) {
            return static_cast<Impl*>(o)->on_sample(hr, flags, ts, smp);
        }));
        ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, m.callback.Get());
    }
    check_hr(MFCreateSourceReaderFromMediaSource(m.source.Get(), ra.Get(), &m.reader), "MFCreateSourceReader");
    if (m.callback) m.callback->set_reader(m.reader.Get());
    m.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    m.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    // Choix du mode natif.
    std::vector<ComPtr<IMFMediaType>> types;
    const auto modes = native_modes(m.reader.Get(), &types);
    log_msg("Carte de capture \"%s\" : %zu modes natifs", narrow(chosen.name).c_str(), modes.size());
    for (const auto& md : modes) log_msg("  %s", cl::describe(md).c_str());
    const int best = cl::choose_mode(modes, req);
    if (best < 0) {
        log_msg(req.automatic() ? "Aucun mode vidéo utilisable (voir la liste ci-dessus)."
                                : "Mode demandé (--mode) introuvable : choisis-en un dans la liste ci-dessus.");
        return false;
    }
    const cl::NativeMode native = modes[size_t(best)];
    check_hr(m.reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, types[size_t(best)].Get()),
             "SetCurrentMediaType (natif)");

    // Format de sortie : NV12 (ou RGB32 si natif RGB / NV12 refusé), converti ensuite en BGRA sur le GPU.
    bool ok = false;
    for (cl::Output o : cl::output_order(native.subtype)) {
        ComPtr<IMFMediaType> t;
        check_hr(MFCreateMediaType(&t), "MFCreateMediaType");
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, o == cl::Output::NV12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
        const HRESULT hr = m.reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, t.Get());
        if (SUCCEEDED(hr)) {
            m.output = o;
            ok       = true;
            break;
        }
        log_msg("Carte de capture : sortie %s refusée par le Source Reader (0x%08lX)", cl::output_name(o),
                static_cast<unsigned long>(hr));
    }
    if (!ok) {
        log_msg("Carte de capture : format natif %s non convertible (ni NV12 ni RGB32) ; essaie un autre --mode.",
                cl::subtype_name(native.subtype));
        return false;
    }
    m.read_format();

    const std::string desc = cl::describe(native);
    m.label = narrow(chosen.name) + " (" + desc + ")";
    log_msg("Carte de capture : mode choisi %s, sortie %s", desc.c_str(), cl::output_name(m.output));

    m.discontinuity = true;
    m.running       = true;
    if (m.sync_read) {
        m.thread = std::thread([this] { impl_->loop(); });
    } else {
        // Première demande ; les suivantes partent du rappel (ReaderCallback::OnReadSample).
        check_hr(m.reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, nullptr, nullptr, nullptr),
                 "ReadSample (asynchrone)");
    }
    log_msg("Carte de capture : lecture %s", m.sync_read ? "synchrone (thread dédié, --capture-sync)" : "asynchrone");
    return true;
}

void MfCaptureSource::stop() {
    auto& m = *impl_;
    m.running = false;
    if (m.callback) {
        // Asynchrone : plus aucun rappel ne touche à la source après detach() (attend au plus le rappel en
        // cours, qui ne bloque pas) ; Flush annule la lecture en attente, attendue au plus 500 ms, jamais plus.
        m.callback->detach();
        if (m.reader && SUCCEEDED(m.reader->Flush(DWORD(MF_SOURCE_READER_ALL_STREAMS))) &&
            WaitForSingleObject(m.callback->flushed(), 500) != WAIT_OBJECT_0)
            log_msg("Carte de capture : Flush non confirmé en 500 ms (arrêt poursuivi)");
    }
    if (m.source) m.source->Shutdown();  // débloque ReadSample (mode synchrone)
    if (m.thread.joinable()) m.thread.join();
    // Tout ce qui référence des objets MF (échantillon gardé, vues sur les textures du pool du décodeur) part
    // avant le reader, la source et le gestionnaire DXGI ; MFShutdown en tout dernier.
    {
        std::lock_guard lk(m.mu);
        m.latest.Reset();
    }
    m.views.clear();
    m.out_view.Reset();
    m.vp.Reset();
    m.en.Reset();
    m.vp_w = m.vp_h = 0;
    m.reader.Reset();
    m.callback.Reset();
    m.source.Reset();
    m.devmgr.Reset();
    m.runtime.reset();
}

bool MfCaptureSource::poll(SourceFrame& out) {
    if (impl_->failed) return false;
    try {
        return poll_impl(out);
    } catch (const std::exception& e) {
        impl_->error = e.what();
        log_msg("Carte de capture : conversion impossible (%s) ; arrêt de la source", e.what());
        impl_->failed = true;
        return false;
    }
}

bool MfCaptureSource::poll_impl(SourceFrame& out) {
    auto& m = *impl_;
    ComPtr<IMFSample> sample;
    double t = 0.0;
    UINT32 w = 0, h = 0;
    bool   full = false, b601 = false;
    LONG   stride = 0;
    {
        std::lock_guard lk(m.mu);
        if (!m.latest) return false;
        sample = std::move(m.latest);
        t      = m.latest_time;
        w      = m.width & ~1u;
        h      = m.height & ~1u;
        full   = m.full_range;
        b601   = m.bt601;
        stride = m.stride;
    }
    if (w < 64 || h < 64) return false;

    const int b = m.basis;
    if (b != m.logged_basis) {
        m.logged_basis = b;
        log_msg("Carte de capture : horodatage = %s", cl::basis_name(cl::TimestampMapper::Basis(b)));
    }

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;
    m.ensure_processor(w, h, full, b601);

    ID3D11Texture2D* in_tex = nullptr;
    UINT             slice  = 0;
    ComPtr<ID3D11Texture2D> gpu_tex;
    ComPtr<IMFDXGIBuffer>   dx;
    const bool gpu = SUCCEEDED(buf.As(&dx)) && SUCCEEDED(dx->GetResource(IID_PPV_ARGS(&gpu_tex)));
    if (gpu) {
        dx->GetSubresourceIndex(&slice);
        in_tex = gpu_tex.Get();
    } else {
        if (!m.upload_system_memory(buf.Get(), w, h, stride)) return false;
        in_tex = m.upload.Get();
    }
    if (!m.logged_path) {
        log_msg("Carte de capture : images %s", gpu ? "livrées sur le GPU (zéro copie)" : "en mémoire système (1 copie)");
        m.logged_path = true;
    }

    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable        = TRUE;
    st.pInputSurface = m.input_view(in_tex, slice);
    check_hr(m.d3d.video_ctx()->VideoProcessorBlt(m.vp.Get(), m.out_view.Get(), 0, 1, &st), "VideoProcessorBlt (capture)");

    out.texture = &m.out;
    out.time    = t;  // instant de capture (horloge now_seconds), pas l'instant du poll
    return true;
}

HANDLE MfCaptureSource::wait_handle() const { return impl_->event; }

bool MfCaptureSource::alive() const { return !impl_->failed; }

std::string MfCaptureSource::name() const { return impl_->label; }
