#include "wgc_capture.hpp"

#include <dwmapi.h>
#include <psapi.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wdx = winrt::Windows::Graphics::DirectX;

// ---------------------------------------------------------------------------------------------
// Énumération des fenêtres

namespace {

std::wstring process_name(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH]{};
    DWORD   n = DWORD(std::size(buf));
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        out.assign(buf, n);
        auto slash = out.find_last_of(L"\\/");
        if (slash != std::wstring::npos) out = out.substr(slash + 1);
    }
    CloseHandle(h);
    return out;
}

BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lp) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(lp);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;

    wchar_t title[512]{};
    int n = GetWindowTextW(hwnd, title, int(std::size(title)));
    if (n <= 0) return TRUE;

    WindowInfo w;
    w.hwnd  = hwnd;
    w.title.assign(title, size_t(n));
    w.process = process_name(hwnd);
    GetWindowRect(hwnd, &w.rect);
    out->push_back(std::move(w));
    return TRUE;
}

}  // namespace

std::vector<WindowInfo> list_capturable_windows() {
    std::vector<WindowInfo> v;
    EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&v));
    return v;
}

HWND find_target_window(const std::wstring& needle, std::wstring* chosen_title) {
    const auto windows = list_capturable_windows();
    std::vector<std::wstring> needles;
    if (needle.empty()) needles = {L"ps remote play", L"chiaki"};
    else needles = {to_lower(needle)};

    for (const auto& n : needles) {
        const WindowInfo* best = nullptr;
        int  best_rank = -1;
        long best_area = -1;
        for (const auto& w : windows) {
            auto t = to_lower(w.title);
            auto pos = t.find(n);
            if (pos == std::wstring::npos) continue;
            int  rank = t == n ? 2 : (pos == 0 ? 1 : 0);
            long area = (w.rect.right - w.rect.left) * (w.rect.bottom - w.rect.top);
            if (rank > best_rank || (rank == best_rank && area > best_area)) {
                best = &w;
                best_rank = rank;
                best_area = area;
            }
        }
        if (best) {
            if (chosen_title) *chosen_title = best->title;
            log_msg("Fenêtre cible : \"%s\" (%s)", narrow(best->title).c_str(), narrow(best->process).c_str());
            return best->hwnd;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Capture

struct WgcSource::Impl {
    Impl(D3DContext& d, bool crop) : d3d(d), aspect_crop(crop) {}

    D3DContext& d3d;
    bool        aspect_crop;
    HWND        target = nullptr;
    HANDLE      event  = nullptr;
    std::string title;
    std::atomic<bool> closed{false};

    wdx::Direct3D11::IDirect3DDevice      device{nullptr};
    wgc::GraphicsCaptureItem              item{nullptr};
    wgc::Direct3D11CaptureFramePool       pool{nullptr};
    wgc::GraphicsCaptureSession           session{nullptr};
    wgc::Direct3D11CaptureFramePool::FrameArrived_revoker arrived;
    wgc::GraphicsCaptureItem::Closed_revoker              closed_rev;
    winrt::Windows::Graphics::SizeInt32   pool_size{};

    GpuTexture crop;
    D3D11_BOX  last_box{};
    bool       warned_small = false;

    bool compute_crop(int content_w, int content_h, D3D11_BOX& box) const;
};

namespace {
wdx::Direct3D11::IDirect3DDevice make_winrt_device(ID3D11Device* device) {
    ComPtr<IDXGIDevice> dxgi;
    check_hr(device->QueryInterface(IID_PPV_ARGS(&dxgi)), "QI IDXGIDevice");
    winrt::com_ptr<::IInspectable> insp;
    check_hr(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), insp.put()), "CreateDirect3D11DeviceFromDXGIDevice");
    return insp.as<wdx::Direct3D11::IDirect3DDevice>();
}
}  // namespace

// Zone client de la fenêtre dans l'image capturée (WGC capture les "extended frame bounds").
bool WgcSource::Impl::compute_crop(int content_w, int content_h, D3D11_BOX& box) const {
    RECT fb{};
    if (FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof(fb))))
        GetWindowRect(target, &fb);
    RECT cr{};
    if (!GetClientRect(target, &cr)) return false;
    POINT org{0, 0};
    ClientToScreen(target, &org);

    int x = std::clamp(int(org.x - fb.left), 0, content_w);
    int y = std::clamp(int(org.y - fb.top), 0, content_h);
    int w = std::min(int(cr.right - cr.left), content_w - x);
    int h = std::min(int(cr.bottom - cr.top), content_h - y);
    if (w <= 0 || h <= 0) return false;

    if (aspect_crop) {  // le flux PS5 est toujours en 16:9 : on enlève les bandes noires
        constexpr double kAspect = 16.0 / 9.0;
        double a = double(w) / double(h);
        if (a > kAspect + 0.01) {
            int nw = int(std::lround(h * kAspect));
            x += (w - nw) / 2;
            w = nw;
        } else if (a < kAspect - 0.01) {
            int nh = int(std::lround(w / kAspect));
            y += (h - nh) / 2;
            h = nh;
        }
    }
    w &= ~1;  // dimensions paires (NV12 pour RTX VSR)
    h &= ~1;
    if (w < 64 || h < 64) return false;
    box = D3D11_BOX{UINT(x), UINT(y), 0, UINT(x + w), UINT(y + h), 1};
    return true;
}

WgcSource::WgcSource(D3DContext& d3d, bool aspect_crop) : impl_(std::make_unique<Impl>(d3d, aspect_crop)) {
    impl_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

WgcSource::~WgcSource() {
    stop();
    if (impl_->event) CloseHandle(impl_->event);
}

bool WgcSource::start(HWND target) {
    if (!target || !IsWindow(target)) return false;
    auto& m = *impl_;
    m.target = target;
    wchar_t title[512]{};
    GetWindowTextW(target, title, int(std::size(title)));
    m.title = narrow(title);

    auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    check_hr(interop->CreateForWindow(target, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(m.item)),
             "GraphicsCaptureItem::CreateForWindow");

    m.device    = make_winrt_device(m.d3d.device());
    m.pool_size = m.item.Size();
    m.pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
        m.device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, m.pool_size);

    HANDLE ev  = m.event;
    m.arrived  = m.pool.FrameArrived(winrt::auto_revoke, [ev](auto&&, auto&&) { SetEvent(ev); });
    m.closed_rev = m.item.Closed(winrt::auto_revoke, [this](auto&&, auto&&) { impl_->closed = true; });

    m.session = m.pool.CreateCaptureSession(m.item);
    try { m.session.IsCursorCaptureEnabled(false); } catch (...) {}
    try { m.session.IsBorderRequired(false); } catch (...) {}  // Windows 11 : pas de cadre jaune
    m.session.StartCapture();
    log_msg("Capture WGC démarrée : %dx%d", m.pool_size.Width, m.pool_size.Height);
    return true;
}

void WgcSource::stop() {
    auto& m = *impl_;
    m.arrived.revoke();
    m.closed_rev.revoke();
    if (m.session) { m.session.Close(); m.session = nullptr; }
    if (m.pool) { m.pool.Close(); m.pool = nullptr; }
    m.item   = nullptr;
    m.device = nullptr;
}

bool WgcSource::poll(SourceFrame& out) {
    auto& m = *impl_;
    if (!m.pool) return false;

    // On vide la file et on ne garde que la plus récente (latence minimale).
    wgc::Direct3D11CaptureFrame latest{nullptr};
    while (auto f = m.pool.TryGetNextFrame()) {
        if (latest) latest.Close();
        latest = f;
    }
    if (!latest) return false;

    const auto cs   = latest.ContentSize();
    const double ts = double(latest.SystemRelativeTime().count()) * 1e-7;

    auto access = latest.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    ComPtr<ID3D11Texture2D> src;
    check_hr(access->GetInterface(IID_PPV_ARGS(&src)), "surface WGC -> ID3D11Texture2D");
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    bool ok = false;
    D3D11_BOX box{};
    if (m.compute_crop(std::min(int(sd.Width), cs.Width), std::min(int(sd.Height), cs.Height), box)) {
        const uint32_t w = box.right - box.left, h = box.bottom - box.top;
        m.crop.ensure(m.d3d.device(), w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "capture crop");
        m.d3d.ctx()->CopySubresourceRegion(m.crop.tex.Get(), 0, 0, 0, 0, src.Get(), 0, &box);
        if (std::memcmp(&box, &m.last_box, sizeof(box)) != 0) {
            log_msg("Zone capturée : %ux%u (offset %u,%u)", w, h, box.left, box.top);
            m.last_box = box;
        }
        out.texture = &m.crop;
        out.time    = ts;
        ok = true;
    } else if (!m.warned_small) {
        log_msg("Fenêtre cible minimisée ou trop petite : capture ignorée");
        m.warned_small = true;
    }
    latest.Close();

    // La fenêtre a changé de taille : on redimensionne le pool (prochaine frame à la bonne taille).
    if (cs.Width != m.pool_size.Width || cs.Height != m.pool_size.Height) {
        m.pool_size = cs;
        m.pool.Recreate(m.device, wdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, cs);
    }
    return ok;
}

HANDLE WgcSource::wait_handle() const { return impl_->event; }

bool WgcSource::alive() const { return !impl_->closed && IsWindow(impl_->target); }

std::string WgcSource::name() const { return impl_->title; }
