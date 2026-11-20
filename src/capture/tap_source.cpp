// Source « frame tap » (format SmoovPlay v2), d'après la spécification et l'émetteur de référence MIT de
// SmoovPlay (docs/frame-tap.md, docs/frame-tap-writer.c ; Copyright (c) 2026 the SmoovPlay authors ;
// THIRD-PARTY-NOTICES.md). Aucun code de chiaki-ng (AGPL) n'est utilisé.
#include "tap_source.hpp"
#include "../gpu/shaders.hpp"

#include <cmath>

TapFrameSource::TapFrameSource(D3DContext& d3d, Compositor& comp, std::wstring name)
    : d3d_(d3d), comp_(comp), name_(name.empty() ? std::wstring(tap::kDefaultName) : std::move(name)) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);  // même base que now_seconds() (util.cpp : QPC / fréquence QPC)
    freq_     = uint64_t(f.QuadPart);
    inv_freq_ = 1.0 / double(f.QuadPart);

    // « Créer, ou ouvrir si elle existe » : l'émetteur peut démarrer avant ou après rpenhance.
    map_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(tap::kTotalBytes >> 32),
                              DWORD(tap::kTotalBytes & 0xFFFFFFFFull), name_.c_str());
    if (!map_) throw_hr(HRESULT_FROM_WIN32(GetLastError()), "CreateFileMappingW (frame tap)");
    view_ = static_cast<uint8_t*>(MapViewOfFile(map_, FILE_MAP_ALL_ACCESS, 0, 0, size_t(tap::kTotalBytes)));
    if (!view_) {  // par exemple une mémoire du même nom, plus petite, créée par une autre appli
        const DWORD e = GetLastError();
        CloseHandle(map_);
        map_ = nullptr;
        throw_hr(HRESULT_FROM_WIN32(e), "MapViewOfFile (frame tap, 4096 + 64 Mio)");
    }
    event_ = CreateEventW(nullptr, FALSE, FALSE, (name_ + L"-frame").c_str());  // auto-reset
    if (!event_) {
        const DWORD e = GetLastError();
        UnmapViewOfFile(view_);
        CloseHandle(map_);
        view_ = nullptr;
        map_  = nullptr;
        throw_hr(HRESULT_FROM_WIN32(e), "CreateEventW (frame tap)");
    }
    reader_  = std::make_unique<tap::Reader>(view_);
    ps_nv12_ = d3d.make_ps(shaders::kTapNv12, "ps_tap_nv12");
    ps_bgra_ = d3d.make_ps(shaders::kTapBgra, "ps_tap_bgra");
    log_msg("Frame tap : mémoire partagée %s (%llu octets), event %s-frame", narrow(name_).c_str(),
            static_cast<unsigned long long>(tap::kTotalBytes), narrow(name_).c_str());
}

TapFrameSource::~TapFrameSource() {
    reader_.reset();
    if (view_) UnmapViewOfFile(view_);
    if (map_) CloseHandle(map_);
    if (event_) CloseHandle(event_);
}

std::string TapFrameSource::name() const {
    std::string n = "Frame tap " + narrow(name_);
    if (reader_->state() == tap::Reader::State::Streaming) {
        const auto& h = reader_->header();
        n += std::string(" (") + tap::codec_name(h.codec) + " " + std::to_string(h.width) + "x" +
             std::to_string(h.height) + ")";
    }
    return n;
}

void TapFrameSource::log_transitions() {
    using S         = tap::Reader::State;
    const S     st  = reader_->state();
    const auto& h   = reader_->header();
    // Une ligne par changement d'état (et par session) : pas de journal à chaque poll pendant l'attente.
    const uint64_t key = st == S::Streaming ? reader_->sessions() : st == S::Unsupported ? h.session : 0;
    if (logged_any_ && st == logged_state_ && key == logged_session_) return;
    logged_any_     = true;
    logged_state_   = st;
    logged_session_ = key;
    switch (st) {
    case S::Waiting:
        log_msg("Frame tap %s : aucun émetteur pour l'instant, en attente (rien n'est affiché tant qu'aucune frame "
                "n'arrive)",
                narrow(name_).c_str());
        break;
    case S::Ended:
        log_msg("Frame tap %s : flux terminé par l'émetteur (session %u), en attente d'un nouveau flux",
                narrow(name_).c_str(), h.session);
        break;
    case S::Unsupported: log_msg("Frame tap %s : %s", narrow(name_).c_str(), reader_->reason().c_str()); break;
    case S::Streaming:
        log_msg("Frame tap %s : session %u, %s %ux%u, %u Hz, %u slots de %u octets, features 0x%X%s%s%s",
                narrow(name_).c_str(), h.session, tap::codec_name(h.codec), h.width, h.height, h.max_fps,
                h.slot_count, h.slot_size, h.features,
                h.codec == tap::kNV12 ? ", couleur " : "",
                h.codec == tap::kNV12 ? tap::colour_name(tap::effective_colour(h.colour, h.height)) : "",
                reader_->reason().empty() ? "" : (" ; " + reader_->reason()).c_str());
        if (h.qpc_freq != 0 && h.qpc_freq != freq_)
            log_msg("Frame tap : qpc_freq de l'en-tête (%llu) différent de celui de cette machine (%llu) ; les "
                    "horodatages sont convertis avec la fréquence locale (même base que l'horloge de rpenhance)",
                    static_cast<unsigned long long>(h.qpc_freq), static_cast<unsigned long long>(freq_));
        if (h.features & 8u)
            log_msg("Frame tap : feature 8 (frames envoyées seulement quand l'image change) : rpenhance n'a pas de "
                    "recalage sur 60 Hz dédié, les frames sont traitées à leur arrivée");
        break;
    }
}

void TapFrameSource::ensure_textures(const tap::Header& h) {
    colour_ = tap::effective_colour(h.colour, h.height);
    yuv_    = tap::yuv_to_rgb(colour_);
    if (h.codec == codec_ && h.width == width_ && h.height == height_ && out_.valid()) return;
    auto* dev  = d3d_.device();
    auto  make = [&](uint32_t w, uint32_t hh, DXGI_FORMAT f, ComPtr<ID3D11Texture2D>& t,
                    ComPtr<ID3D11ShaderResourceView>& s, const char* what) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width            = w;
        d.Height           = hh;
        d.MipLevels        = 1;
        d.ArraySize        = 1;
        d.Format           = f;
        d.SampleDesc.Count = 1;
        d.Usage            = D3D11_USAGE_DYNAMIC;
        d.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;
        t.Reset();
        s.Reset();
        check_hr(dev->CreateTexture2D(&d, nullptr, &t), what);
        check_hr(dev->CreateShaderResourceView(t.Get(), nullptr, &s), what);
    };
    y_.Reset();
    uv_.Reset();
    bgra_.Reset();
    y_srv_.Reset();
    uv_srv_.Reset();
    bgra_srv_.Reset();
    if (h.codec == tap::kNV12) {
        make(h.width, h.height, DXGI_FORMAT_R8_UNORM, y_, y_srv_, "frame tap Y");
        make(h.width / 2, h.height / 2, DXGI_FORMAT_R8G8_UNORM, uv_, uv_srv_, "frame tap CbCr");
    } else {
        make(h.width, h.height, DXGI_FORMAT_B8G8R8A8_UNORM, bgra_, bgra_srv_, "frame tap BGRA");
    }
    out_.ensure(dev, h.width, h.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "frame tap out");
    codec_  = h.codec;
    width_  = h.width;
    height_ = h.height;
}

// Copie directe du slot dans les textures dynamiques (une seule copie CPU). Si le slot est réécrit pendant la
// copie, la frame est abandonnée sans être convertie : out_ garde la frame précédente.
void TapFrameSource::upload(const uint8_t* data, size_t bytes) {
    auto* ctx   = d3d_.ctx();
    auto  plane = [&](ID3D11Texture2D* t, size_t offset, uint32_t row_bytes, uint32_t rows, uint8_t fill) {
        D3D11_MAPPED_SUBRESOURCE m{};
        check_hr(ctx->Map(t, 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map frame tap");
        auto* dst = static_cast<uint8_t*>(m.pData);
        for (uint32_t r = 0; r < rows; ++r) {
            uint8_t*     drow  = dst + size_t(r) * m.RowPitch;
            const size_t s     = offset + size_t(r) * row_bytes;
            const size_t avail = s < bytes ? std::min<size_t>(row_bytes, bytes - s) : 0;
            if (avail) std::memcpy(drow, data + s, avail);
            if (avail < row_bytes) std::memset(drow + avail, fill, row_bytes - avail);  // frame tronquée
        }
        ctx->Unmap(t, 0);
    };
    if (codec_ == tap::kNV12) {
        plane(y_.Get(), 0, width_, height_, 16);
        plane(uv_.Get(), size_t(width_) * height_, width_, height_ / 2, 128);  // w/2 paires Cb, Cr par ligne
    } else {
        plane(bgra_.Get(), 0, width_ * 4, height_, 0);
    }
}

void TapFrameSource::convert() {
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(width_), float(height_), 0.0f, 1.0f};
    comp_.begin(out_.rtv.Get(), vp);
    if (codec_ == tap::kNV12) {
        struct {
            float r[4], g[4], b[4], size[4];
        } cb{};
        static_assert(sizeof(cb) == 64);
        for (int j = 0; j < 4; ++j) {
            cb.r[j] = yuv_.m[0][j];
            cb.g[j] = yuv_.m[1][j];
            cb.b[j] = yuv_.m[2][j];
        }
        cb.size[0] = float(width_);
        cb.size[1] = float(height_);
        cb.size[2] = float(width_ / 2);
        cb.size[3] = float(height_ / 2);
        comp_.draw(ps_nv12_.Get(), {y_srv_.Get(), uv_srv_.Get()}, d3d_.constants(cb));
    } else {
        comp_.draw(ps_bgra_.Get(), {bgra_srv_.Get()});
    }
    comp_.end();
}

double TapFrameSource::to_seconds(uint64_t qpc) {
    const double now = now_seconds();
    if (qpc == 0) return now;
    const double t = double(qpc) * inv_freq_;  // même calcul que now_seconds()
    if (t > now + 0.5 || t < now - 5.0) {      // horodatage d'une autre horloge : inutilisable
        if (!warned_time_)
            log_msg("Frame tap : qpc_arrival incohérent (%.3f s pour une horloge à %.3f s) : instant de lecture utilisé",
                    t, now);
        warned_time_ = true;
        return now;
    }
    return t;
}

bool TapFrameSource::poll(SourceFrame& out) {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    reader_->heartbeat(uint64_t(q.QuadPart));
    reader_->refresh();
    log_transitions();
    if (reader_->state() != tap::Reader::State::Streaming) return false;

    const tap::Header& h = reader_->header();
    ensure_textures(h);
    tap::SlotMeta m;
    const auto r = reader_->read_latest(m, [&](const uint8_t* d, size_t n) { upload(d, n); });
    if (r != tap::Reader::Read::Frame) return false;
    convert();

    out.texture          = &out_;
    out.time             = to_seconds(m.qpc_arrival);
    out.meta             = SourceFrameMeta{};
    out.meta.valid       = true;
    out.meta.repeat      = (m.flags & tap::kFlagRepeat) != 0;
    out.meta.fresh       = (m.flags & tap::kFlagNew) != 0;
    out.meta.truncated   = m.truncated();
    out.meta.frames_lost = m.frames_lost;
    out.meta.recovered   = m.recovered != 0;
    out.meta.new_session = reader_->take_new_session();
    out.meta.session     = h.session;
    out.meta.seq         = m.seq;
    return true;
}
