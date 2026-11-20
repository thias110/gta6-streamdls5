#include "capture_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

namespace capture_logic {

double TimestampMapper::map(std::optional<int64_t> device_qpc_hns, std::optional<int64_t> sample_hns,
                            bool discontinuity, double arrival) {
    const double t = map_raw(device_qpc_hns, sample_hns, discontinuity, arrival);
    // Garde-fou : jamais de retour en arrière ni d'égalité entre deux images d'un même flux (l'interpolation
    // divise par l'écart) ; jamais après l'arrivée. Une discontinuité autorise un recalage.
    double out = t;
    if (have_out_ && !discontinuity && out <= last_out_) out = std::min(arrival, last_out_ + kMinStep);
    have_out_ = true;
    last_out_ = out;
    return out;
}

double TimestampMapper::map_raw(std::optional<int64_t> device_qpc_hns, std::optional<int64_t> sample_hns,
                                bool discontinuity, double arrival) {
    if (device_qpc_hns && *device_qpc_hns > 0) {
        const double t   = hns_to_seconds(*device_qpc_hns);
        const double lag = arrival - t;
        if (lag >= -kFutureSlack && lag <= kMaxLag) {
            basis_ = Basis::Device;
            return std::min(t, arrival);
        }
    }
    if (sample_hns && *sample_hns >= 0) {
        const double s = hns_to_seconds(*sample_hns);
        // Horodatage figé (pilote qui renvoie toujours 0 ou la même valeur) : inutilisable, on garde l'ancre.
        if (anchored_ && !discontinuity && s == last_s_) {
            basis_ = Basis::Arrival;
            return arrival;
        }
        const double candidate = arrival - s;
        if (!anchored_ || discontinuity || s < last_s_ || s - last_s_ > kResyncGap) {
            offset_   = candidate;
            anchored_ = true;
        } else if (candidate < offset_) {
            offset_ = candidate;  // arrivée plus rapide que toutes les précédentes : nouvelle borne basse
        } else {
            // Remontée lente : suit une dérive entre l'horloge de la carte et QPC, et oublie une borne basse
            // aberrante, sans réintroduire la gigue de livraison.
            offset_ = std::min(candidate, offset_ + kDriftPerSecond * (s - last_s_));
        }
        last_s_ = s;
        basis_  = Basis::Mapped;
        return std::min(s + offset_, arrival);
    }
    basis_ = Basis::Arrival;
    return arrival;
}

const char* basis_name(TimestampMapper::Basis b) {
    switch (b) {
    case TimestampMapper::Basis::Device: return "horloge QPC du pilote (MFSampleExtension_DeviceReferenceSystemTime)";
    case TimestampMapper::Basis::Mapped: return "horodatage MF recalé sur l'arrivée";
    case TimestampMapper::Basis::Arrival: return "instant d'arrivée (aucun horodatage)";
    default: return "aucun";
    }
}

const char* subtype_name(Subtype s) {
    switch (s) {
    case Subtype::NV12: return "NV12";
    case Subtype::YUY2: return "YUY2";
    case Subtype::MJPG: return "MJPEG";
    case Subtype::RGB32: return "RGB32";
    case Subtype::P010: return "P010";
    case Subtype::H264: return "H264";
    case Subtype::HEVC: return "HEVC";
    default: return "autre";
    }
}

bool is_uncompressed(Subtype s) {
    return s == Subtype::NV12 || s == Subtype::YUY2 || s == Subtype::RGB32 || s == Subtype::P010;
}

std::string describe(const NativeMode& m) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%ux%u @ %.2f fps, %s", m.width, m.height, m.fps, subtype_name(m.subtype));
    return buf;
}

bool parse_mode(std::string_view text, ModeRequest& out, std::string* error) {
    out = ModeRequest{};
    if (text.empty()) return true;
    auto fail = [&] {
        if (error) *error = "--mode attend LARGEURxHAUTEUR ou LARGEURxHAUTEUR@FPS, par exemple 1920x1080@60";
        out = ModeRequest{};
        return false;
    };
    const std::string s(text);
    const char* p   = s.c_str();
    char*       end = nullptr;
    const unsigned long w = std::strtoul(p, &end, 10);
    if (end == p || (*end != 'x' && *end != 'X')) return fail();
    p = end + 1;
    const unsigned long h = std::strtoul(p, &end, 10);
    if (end == p) return fail();
    double fps = 0.0;
    if (*end == '@') {
        p   = end + 1;
        fps = std::strtod(p, &end);
        if (end == p || !(fps > 0.0) || fps > 1000.0) return fail();
    }
    if (*end != '\0') return fail();
    if (w < 64 || h < 64 || w > 16384 || h > 16384) return fail();
    out.width  = uint32_t(w);
    out.height = uint32_t(h);
    out.fps    = fps;
    return true;
}

int choose_mode(const std::vector<NativeMode>& modes, const ModeRequest& req) {
    int    best       = -1;
    double best_score = -1.0;
    for (size_t i = 0; i < modes.size(); ++i) {
        const NativeMode& m = modes[i];
        if (m.width < 64 || m.height < 64) continue;  // inutilisable par le pipeline
        double score;
        if (!req.automatic()) {
            const bool match = m.width == req.width && m.height == req.height &&
                               (req.fps <= 0.0 || std::abs(m.fps - req.fps) < 1.0);
            if (!match) continue;
            score = 1.0 + (is_uncompressed(m.subtype) ? 1.0 : 0.0);
        } else {
            const double px = double(std::min<uint64_t>(uint64_t(m.width) * m.height, 3840ull * 2160ull));
            score = (m.fps >= 59.0 ? 1e13 : 0.0) + px * 1e3 + std::min(m.fps, 144.0) * 10.0 +
                    (is_uncompressed(m.subtype) ? 5.0 : 0.0);
        }
        if (score > best_score) {
            best_score = score;
            best       = int(i);
        }
    }
    return best;
}

const char* output_name(Output o) { return o == Output::NV12 ? "NV12" : "RGB32"; }

std::vector<Output> output_order(Subtype native) {
    if (native == Subtype::RGB32) return {Output::RGB32, Output::NV12};
    return {Output::NV12, Output::RGB32};
}

namespace {
std::wstring lower(std::wstring_view s) {
    std::wstring r(s);
    for (auto& c : r) c = wchar_t(std::towlower(wint_t(c)));
    return r;
}
}  // namespace

int pick_device(const std::vector<std::wstring>& names, std::wstring_view needle) {
    if (names.empty()) return -1;
    if (!needle.empty()) {
        const std::wstring n = lower(needle);
        for (size_t i = 0; i < names.size(); ++i)
            if (lower(names[i]).find(n) != std::wstring::npos) return int(i);
        return -1;
    }
    static const wchar_t* kHints[] = {L"elgato", L"avermedia", L"capture", L"hdmi", L"cam link", L"live gamer",
                                      L"magewell", L"usb video", L"uvc"};
    for (const wchar_t* hint : kHints)
        for (size_t i = 0; i < names.size(); ++i)
            if (lower(names[i]).find(hint) != std::wstring::npos) return int(i);
    return 0;
}

}  // namespace capture_logic
