#pragma once
#include "frame_source.hpp"

struct WindowInfo {
    HWND         hwnd = nullptr;
    std::wstring title;
    std::wstring process;
    RECT         rect{};
};

std::vector<WindowInfo> list_capturable_windows();
// Cherche une fenêtre dont le titre contient `needle` (insensible à la casse).
// needle vide : essaie "PS Remote Play" puis "chiaki". Préfère titre exact > préfixe > contient, puis la plus grande.
HWND find_target_window(const std::wstring& needle, std::wstring* chosen_title = nullptr);

// Windows Graphics Capture d'une fenêtre, recadrée sur sa zone client (et en 16:9 si demandé).
class WgcSource final : public FrameSource {
public:
    WgcSource(D3DContext& d3d, bool aspect_crop);
    ~WgcSource() override;

    bool start(HWND target);
    void stop();

    bool        poll(SourceFrame& out) override;
    HANDLE      wait_handle() const override;
    bool        alive() const override;
    std::string name() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
