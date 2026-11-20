#pragma once
#include "frame_source.hpp"

// Noms des périphériques vidéo vus par Media Foundation (cartes de capture UVC, webcams). Lève en cas d'échec MF.
std::vector<std::wstring> list_capture_devices();
// Modes natifs du périphérique choisi comme le ferait --source capture (needle vide = détection auto) ;
// false si aucun périphérique ne correspond. Lève en cas d'échec MF.
bool list_capture_modes(const std::wstring& needle, std::wstring& device, std::vector<std::string>& modes);

// Carte de capture HDMI (PS5 branchée en HDMI) via Media Foundation : `--source capture` (alias `--hdmi`).
// - Choisit le meilleur mode natif : >= 60 fps d'abord, puis la plus grande résolution (<= 4K),
//   puis un format non compressé (moins de latence que le MJPEG) ; ou le mode demandé (--mode).
// - Demande du NV12 au Source Reader (qui décode/convertit YUY2, MJPEG, P010, H.264...), du RGB32 si le natif
//   est RGB ou si le NV12 est refusé ; si la carte livre des textures GPU (gestionnaire DXGI), aucune copie
//   CPU, sinon une seule copie vers le GPU.
// - Conversion -> BGRA par le VideoProcessor D3D11 (couleurs BT.601/709, plage limitée/pleine lues dans le
//   type média), conforme au contrat de SourceFrame.
// - ReadSample tourne sur un thread dédié qui ne garde que l'image la plus récente et signale wait_handle() ;
//   poll() ne bloque jamais (thread GPU/UI).
// - SourceFrame::time = instant de capture sur l'horloge now_seconds() : horloge QPC du pilote si fournie,
//   sinon horodatage MF (100 ns) recalé sur l'arrivée (voir capture_logic::TimestampMapper).
class MfCaptureSource final : public FrameSource {
public:
    explicit MfCaptureSource(D3DContext& d3d);
    ~MfCaptureSource() override;
    MfCaptureSource(const MfCaptureSource&)            = delete;
    MfCaptureSource& operator=(const MfCaptureSource&) = delete;

    // needle : sous-chaîne du nom du périphérique (vide = détection auto) ;
    // mode : "1920x1080@60" ou "1920x1080" (vide = auto). Ne lève pas : false + journal en cas d'échec.
    bool start(const std::wstring& needle, const std::wstring& mode);
    void stop();

    bool        poll(SourceFrame& out) override;
    HANDLE      wait_handle() const override;
    bool        alive() const override;  // faux : carte débranchée, fin de flux ou conversion impossible
    std::string name() const override;

private:
    bool start_impl(const std::wstring& needle, const std::wstring& mode);
    bool poll_impl(SourceFrame& out);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};
