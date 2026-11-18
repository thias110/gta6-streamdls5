#include "mf_capture.hpp"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cmath>
#include <cwchar>
#include <mutex>
#include <thread>

namespace {

// MFStartup/MFShutdown comptés en références.
struct MfRuntime {
    MfRuntime() { check_hr(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET), "MFStartup"); }
    ~MfRuntime() { MFShutdown(); }
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
    std::vector<Device> out;
    for (UINT32 i = 0; i < n; ++i) {
        Device d;
        d.activate.Attach(arr[i]);
        WCHAR* name = nullptr;
        UINT32 len  = 0;
        if (SUCCEEDED(d.activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
            d.name.assign(name, len);
            CoTaskMemFree(name);
        }
        out.push_back(std::move(d));
    }
    CoTaskMemFree(arr);
    return out;
}

const char* subtype_name(const GUID& g) {
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFVideoFormat_MJPG) return "MJPEG";
    if (g == MFVideoFormat_RGB32) return "RGB32";
    if (g == MFVideoFormat_P010) return "P010";
    if (g == MFVideoFormat_H264) return "H264";
    if (g == MFVideoFormat_HEVC) return "HEVC";
    return "autre";
}

bool is_uncompressed(const GUID& g) {
    return g == MFVideoFormat_NV12 || g == MFVideoFormat_YUY2 || g == MFVideoFormat_RGB32 || g == MFVideoFormat_P010;
}

}  // namespace

std::vector<std::wstring> list_capture_devices() {
    MfRuntime rt;
    std::vector<std::wstring> names;
    for (auto& d : enum_devices()) names.push_back(d.name);
    return names;
}

// ---------------------------------------------------------------------------------------------

struct MfCaptureSource::Impl {
    explicit Impl(D3DContext& d) : d3d(d) {}

    D3DContext&                 d3d;
    std::unique_ptr<MfRuntime>  runtime;
    ComPtr<IMFDXGIDeviceManager> devmgr;
    ComPtr<IMFMediaSource>      source;
    ComPtr<IMFSourceReader>     reader;

    std::thread       thread;
    std::atomic<bool> running{false};
    std::atomic<bool> failed{false};
    HANDLE            event = nullptr;

    std::mutex        mu;              // protège latest*, width, height, colour
    ComPtr<IMFSample> latest;
    double            latest_time = 0.0;
    UINT32            width = 0, height = 0;
    bool              full_range = false, bt601 = false;
    std::string       label;

    // Conversion NV12 -> BGRA (thread de rendu uniquement)
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
    ComPtr<ID3D11Texture2D>                staging, upload;  // chemin mémoire système
    bool                                   logged_path = false;

    void read_format();
    void loop();
    bool ensure_processor(UINT32 w, UINT32 h, bool full, bool b601);
    ID3D11VideoProcessorInputView* input_view(ID3D11Texture2D* tex, UINT slice);
    bool upload_system_memory(IMFMediaBuffer* buf, UINT32 w, UINT32 h);
};

void MfCaptureSource::Impl::read_format() {
    ComPtr<IMFMediaType> cur;
    check_hr(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &cur), "GetCurrentMediaType");
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h);
    const UINT32 range  = MFGetAttributeUINT32(cur.Get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    const UINT32 matrix = MFGetAttributeUINT32(cur.Get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    std::lock_guard lk(mu);
    width      = w;
    height     = h;
    full_range = range == MFNominalRange_0_255;
    bt601      = matrix == MFVideoTransferMatrix_BT601;
}

void MfCaptureSource::Impl::loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (running) {
        DWORD             stream = 0, flags = 0;
        LONGLONG          ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &stream, &flags, &ts, &sample);
        if (FAILED(hr)) {
            if (running) log_msg("Carte de capture : ReadSample en échec (0x%08lX)", static_cast<unsigned long>(hr));
            failed = true;
            break;
        }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
            log_msg("Carte de capture : fin du flux (débranchée ?)");
            failed = true;
            break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            read_format();
            log_msg("Carte de capture : nouveau format %ux%u", width, height);
        }
        if (!sample) continue;
        {
            std::lock_guard lk(mu);
            latest      = sample;
            latest_time = now_seconds();
        }
        SetEvent(event);
    }
    CoUninitialize();
}

bool MfCaptureSource::Impl::ensure_processor(UINT32 w, UINT32 h, bool full, bool b601) {
    if (vp && vp_w == w && vp_h == h && vp_full == full && vp_601 == b601) return true;
    auto* vdev = d3d.video_device();
    auto* vctx = d3d.video_ctx();
    if (!vdev || !vctx) throw std::runtime_error("API vidéo D3D11 indisponible : conversion NV12 impossible");

    views.clear();
    out_view.Reset();
    vp.Reset();
    en.Reset();

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

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
    yuv.YCbCr_Matrix  = b601 ? 0 : 1;
    yuv.Nominal_Range = full ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
    rgb.RGB_Range = 0;
    const RECT r{0, 0, LONG(w), LONG(h)};
    vctx->VideoProcessorSetStreamFrameFormat(vp.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vp.Get(), 0, FALSE);
    vctx->VideoProcessorSetStreamColorSpace(vp.Get(), 0, &yuv);
    vctx->VideoProcessorSetOutputColorSpace(vp.Get(), &rgb);
    vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &r);
    vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &r);
    vctx->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &r);

    vp_w    = w;
    vp_h    = h;
    vp_full = full;
    vp_601  = b601;
    log_msg("Carte de capture : conversion NV12 %ux%u (BT.%s, plage %s) -> BGRA", w, h, b601 ? "601" : "709",
            full ? "pleine" : "limitée");
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

bool MfCaptureSource::Impl::upload_system_memory(IMFMediaBuffer* buf, UINT32 w, UINT32 h) {
    auto* dev = d3d.device();
    auto* ctx = d3d.ctx();
    D3D11_TEXTURE2D_DESC have{};
    if (staging) staging->GetDesc(&have);
    if (!staging || have.Width != w || have.Height != h) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = h;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = DXGI_FORMAT_NV12;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_STAGING;
        d.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
        staging.Reset();
        upload.Reset();
        check_hr(dev->CreateTexture2D(&d, nullptr, &staging), "NV12 staging");
        d.Usage          = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        d.BindFlags      = D3D11_BIND_RENDER_TARGET;
        check_hr(dev->CreateTexture2D(&d, nullptr, &upload), "NV12 upload");
    }

    BYTE* src   = nullptr;
    LONG  pitch = 0;
    ComPtr<IMF2DBuffer> b2;
    DWORD len = 0;
    const bool two_d = SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2)));
    if (two_d) {
        if (FAILED(b2->Lock2D(&src, &pitch))) return false;
    } else {
        if (FAILED(buf->Lock(&src, nullptr, &len))) return false;
        pitch = LONG(w);
    }
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(staging.Get(), 0, D3D11_MAP_WRITE, 0, &m);
    if (SUCCEEDED(hr)) {
        auto* dst = static_cast<BYTE*>(m.pData);
        for (UINT32 y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * m.RowPitch, src + size_t(y) * pitch, w);
        const BYTE* uv_src = src + size_t(pitch) * h;
        BYTE*       uv_dst = dst + size_t(m.RowPitch) * h;
        for (UINT32 y = 0; y < h / 2; ++y)
            std::memcpy(uv_dst + size_t(y) * m.RowPitch, uv_src + size_t(y) * pitch, w);
        ctx->Unmap(staging.Get(), 0);
        ctx->CopyResource(upload.Get(), staging.Get());
    }
    if (two_d) b2->Unlock2D();
    else buf->Unlock();
    return SUCCEEDED(hr);
}

// ---------------------------------------------------------------------------------------------

MfCaptureSource::MfCaptureSource(D3DContext& d3d) : impl_(std::make_unique<Impl>(d3d)) {
    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

MfCaptureSource::~MfCaptureSource() {
    stop();
    if (impl_->event) CloseHandle(impl_->event);
}

bool MfCaptureSource::start(const std::wstring& needle, const std::wstring& mode) {
    auto& m   = *impl_;
    m.runtime = std::make_unique<MfRuntime>();

    auto devices = enum_devices();
    if (devices.empty()) {
        log_msg("Aucune carte de capture / périphérique vidéo détecté.");
        return false;
    }
    const Device* chosen = nullptr;
    if (!needle.empty()) {
        const auto n = to_lower(needle);
        for (auto& d : devices)
            if (to_lower(d.name).find(n) != std::wstring::npos) { chosen = &d; break; }
    } else {
        static const wchar_t* kHints[] = {L"elgato", L"avermedia", L"capture", L"hdmi", L"cam link", L"live gamer",
                                          L"magewell", L"usb video", L"uvc"};
        for (auto* hint : kHints) {
            for (auto& d : devices)
                if (to_lower(d.name).find(hint) != std::wstring::npos) { chosen = &d; break; }
            if (chosen) break;
        }
        if (!chosen) chosen = &devices.front();
    }
    if (!chosen) {
        log_msg("Aucun périphérique ne contient \"%s\".", narrow(needle).c_str());
        return false;
    }

    UINT32 req_w = 0, req_h = 0;
    double req_fps = 0.0;
    if (!mode.empty() && std::swscanf(mode.c_str(), L"%ux%u@%lf", &req_w, &req_h, &req_fps) < 2)
        throw std::invalid_argument("--mode attend LARGEURxHAUTEUR@FPS, par exemple 1920x1080@60");

    check_hr(chosen->activate->ActivateObject(IID_PPV_ARGS(&m.source)), "ActivateObject (carte de capture)");

    UINT token = 0;
    check_hr(MFCreateDXGIDeviceManager(&token, &m.devmgr), "MFCreateDXGIDeviceManager");
    check_hr(m.devmgr->ResetDevice(m.d3d.device(), token), "IMFDXGIDeviceManager::ResetDevice");

    ComPtr<IMFAttributes> ra;
    check_hr(MFCreateAttributes(&ra, 4), "MFCreateAttributes reader");
    ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, m.devmgr.Get());
    ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ra->SetUINT32(MF_LOW_LATENCY, TRUE);
    check_hr(MFCreateSourceReaderFromMediaSource(m.source.Get(), ra.Get(), &m.reader), "MFCreateSourceReader");
    m.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    m.reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);

    // Choix du mode natif.
    ComPtr<IMFMediaType> best;
    double best_score = -1.0;
    std::string best_desc;
    std::vector<std::string> all_modes;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        HRESULT hr = m.reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &t);
        if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) break;
        GUID   sub{};
        UINT32 w = 0, h = 0, num = 0, den = 0;
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &num, &den);
        const double fps = den ? double(num) / double(den) : 0.0;
        char desc[96];
        std::snprintf(desc, sizeof(desc), "%ux%u @ %.2f fps, %s", w, h, fps, subtype_name(sub));
        all_modes.emplace_back(desc);

        double score;
        if (req_w) {
            const bool match = w == req_w && h == req_h && (req_fps <= 0.0 || std::abs(fps - req_fps) < 1.0);
            score = match ? 1.0 + (is_uncompressed(sub) ? 1.0 : 0.0) : -1.0;
        } else {
            const double px = double(std::min<uint64_t>(uint64_t(w) * h, 3840ull * 2160ull));
            score = (fps >= 59.0 ? 1e13 : 0.0) + px * 1e3 + std::min(fps, 144.0) * 10.0 +
                    (is_uncompressed(sub) ? 5.0 : 0.0);
        }
        if (score > best_score) {
            best_score = score;
            best       = t;
            best_desc  = desc;
        }
    }
    log_msg("Carte de capture \"%s\" : %zu modes natifs", narrow(chosen->name).c_str(), all_modes.size());
    if (!best) {
        log_msg(req_w ? "Mode demandé introuvable. Modes disponibles :" : "Aucun mode vidéo utilisable. Modes :");
        for (const auto& d : all_modes) log_msg("  %s", d.c_str());
        return false;
    }
    check_hr(m.reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, best.Get()),
             "SetCurrentMediaType (natif)");

    ComPtr<IMFMediaType> nv12;
    check_hr(MFCreateMediaType(&nv12), "MFCreateMediaType");
    nv12->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    nv12->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    check_hr(m.reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, nv12.Get()),
             "SetCurrentMediaType (NV12)");
    m.read_format();

    m.label = narrow(chosen->name) + " (" + best_desc + ")";
    log_msg("Carte de capture : mode choisi %s", best_desc.c_str());

    m.running = true;
    m.thread  = std::thread([this] { impl_->loop(); });
    return true;
}

void MfCaptureSource::stop() {
    auto& m = *impl_;
    m.running = false;
    if (m.source) m.source->Shutdown();  // débloque ReadSample
    if (m.thread.joinable()) m.thread.join();
    m.reader.Reset();
    m.source.Reset();
    m.devmgr.Reset();
    m.runtime.reset();
}

bool MfCaptureSource::poll(SourceFrame& out) {
    auto& m = *impl_;
    ComPtr<IMFSample> sample;
    double t = 0.0;
    UINT32 w = 0, h = 0;
    bool   full = false, b601 = false;
    {
        std::lock_guard lk(m.mu);
        if (!m.latest) return false;
        sample = std::move(m.latest);
        t      = m.latest_time;
        w      = m.width & ~1u;
        h      = m.height & ~1u;
        full   = m.full_range;
        b601   = m.bt601;
    }
    if (w < 64 || h < 64) return false;

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
        if (!m.upload_system_memory(buf.Get(), w, h)) return false;
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
    out.time    = t;
    return true;
}

HANDLE MfCaptureSource::wait_handle() const { return impl_->event; }

bool MfCaptureSource::alive() const { return !impl_->failed; }

std::string MfCaptureSource::name() const { return impl_->label; }
