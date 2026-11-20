#pragma once
// Source de frames « frame tap » (format SmoovPlay version 2 : frames décodées NV12 ou BGRA en mémoire
// partagée), écrite d'après la spécification MIT de SmoovPlay (docs/frame-tap.md) et son émetteur de
// référence MIT (docs/frame-tap-writer.c) — Copyright (c) 2026 the SmoovPlay authors, voir
// THIRD-PARTY-NOTICES.md. Aucun code de chiaki-ng (AGPL) n'est utilisé.
//
// Thread GPU unique : poll() copie le slot le plus récent dans des textures dynamiques puis convertit en BGRA
// (shader) dans une texture SRV + RT conforme au contrat de SourceFrame.
#include "../pipeline/compositor.hpp"
#include "frame_source.hpp"
#include "tap_protocol.hpp"

class TapFrameSource final : public FrameSource {
public:
    // name : nom de la mémoire partagée (défaut tap::kDefaultName). Crée ou ouvre la mémoire et l'event
    // `<name>-frame` : l'émetteur peut démarrer avant ou après. Lève std::runtime_error en cas d'échec.
    TapFrameSource(D3DContext& d3d, Compositor& comp, std::wstring name);
    ~TapFrameSource() override;
    TapFrameSource(const TapFrameSource&)            = delete;
    TapFrameSource& operator=(const TapFrameSource&) = delete;

    bool        poll(SourceFrame& out) override;
    HANDLE      wait_handle() const override { return event_; }
    std::string name() const override;

    tap::Reader::State state() const { return reader_->state(); }
    const tap::Reader& reader() const { return *reader_; }

private:
    void ensure_textures(const tap::Header& h);
    void upload(const uint8_t* data, size_t bytes);
    void convert();
    void log_transitions();
    double to_seconds(uint64_t qpc);

    D3DContext&  d3d_;
    Compositor&  comp_;
    std::wstring name_;
    HANDLE       map_   = nullptr;
    HANDLE       event_ = nullptr;
    uint8_t*     view_  = nullptr;
    std::unique_ptr<tap::Reader> reader_;

    ComPtr<ID3D11PixelShader>        ps_nv12_, ps_bgra_;
    ComPtr<ID3D11Texture2D>          y_, uv_, bgra_;  // dynamiques (écriture CPU)
    ComPtr<ID3D11ShaderResourceView> y_srv_, uv_srv_, bgra_srv_;
    GpuTexture                       out_;            // BGRA, SRV + RT
    uint32_t                         codec_ = 0, width_ = 0, height_ = 0, colour_ = 0;
    tap::YuvToRgb                    yuv_{};

    double   inv_freq_ = 0.0;
    uint64_t freq_     = 0;
    tap::Reader::State logged_state_   = tap::Reader::State::Waiting;
    bool               logged_any_     = false;
    uint64_t           logged_session_ = ~0ull;
    bool               warned_time_    = false;
};
