#pragma once
#include "frame_source.hpp"

// Noms des périphériques vidéo vus par Media Foundation (cartes de capture UVC, webcams).
std::vector<std::wstring> list_capture_devices();

// Carte de capture HDMI (PS5 branchée en HDMI) via Media Foundation.
// - Choisit le meilleur mode natif : >= 60 fps d'abord, puis la plus grande résolution (<= 4K),
//   puis un format non compressé (moins de latence que le MJPEG).
// - Demande du NV12 au Source Reader ; si la carte livre des textures GPU (gestionnaire DXGI),
//   aucune copie CPU, sinon une seule copie vers le GPU.
// - Conversion NV12 -> BGRA par le VideoProcessor D3D11 (couleurs BT.601/709, plage limitée/pleine
//   lues dans le type média).
class MfCaptureSource final : public FrameSource {
public:
    explicit MfCaptureSource(D3DContext& d3d);
    ~MfCaptureSource() override;

    // needle : sous-chaîne du nom du périphérique (vide = détection auto) ;
    // mode : "1920x1080@60" (vide = auto).
    bool start(const std::wstring& needle, const std::wstring& mode);
    void stop();

    bool        poll(SourceFrame& out) override;
    HANDLE      wait_handle() const override;
    bool        alive() const override;
    std::string name() const override;

private:
    bool start_impl(const std::wstring& needle, const std::wstring& mode);
    bool poll_impl(SourceFrame& out);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
