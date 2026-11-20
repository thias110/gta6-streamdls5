#include "scene_cut.hpp"

#include <algorithm>
#include <cmath>

namespace {
// Écart-type (luminance 0..1) sous lequel une image est considérée unie (écran noir, fondu) :
// sa corrélation n'aurait pas de sens.
constexpr double kFlatStd = 0.004;
}  // namespace

CutFeatures cut_features(const CutStatsRaw& r) {
    CutFeatures f;
    if (r.n < 1.0f) return f;
    const double n  = r.n;
    const double ma = r.sa / n, mb = r.sb / n;
    const double sa = std::sqrt(std::max(0.0, r.saa / n - ma * ma));
    const double sb = std::sqrt(std::max(0.0, r.sbb / n - mb * mb));
    f.valid = true;

    const bool flat_a = sa < kFlatStd, flat_b = sb < kFlatStd;
    if (flat_a && flat_b) {
        f.corr = 1.0f;  // deux images unies : même « disposition »
    } else if (flat_a || flat_b) {
        f.corr = 0.0f;  // une structure apparaît ou disparaît
    } else {
        double best = -1.0;
        for (int k = 0; k < kCutShifts; ++k) best = std::max(best, (r.sab[k] / n - ma * mb) / (sa * sb));
        f.corr = float(std::clamp(best, -1.0, 1.0));
    }

    uint64_t na = 0, nb = 0;
    for (int i = 0; i < kCutBins; ++i) {
        na += r.ha[i];
        nb += r.hb[i];
    }
    if (na && nb) {
        double tv = 0.0;
        for (int i = 0; i < kCutBins; ++i) tv += std::abs(double(r.ha[i]) / double(na) - double(r.hb[i]) / double(nb));
        f.hist = float(0.5 * tv);
    }
    return f;
}

bool is_scene_cut(float mean_diff, const CutFeatures& f, const CutParams& p) {
    if (mean_diff > p.mean_max) return true;
    if (!p.enabled || !f.valid) return false;
    if (f.corr < p.corr_strong) return true;
    return f.corr < p.corr_max && f.hist > p.hist_min;
}
