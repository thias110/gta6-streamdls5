#include "dedupe.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kFloor     = 0.15f;  // seuil minimal absolu (0-255)
constexpr float kMinRatio  = 2.5f;   // écart minimal (rapport des médianes) pour croire à deux populations
constexpr float kSmoothing = 0.15f;  // vitesse de glissement du seuil (par capture)
constexpr float kAmbiguity = 2.0f;   // métrique dans [seuil / k, seuil * k] : le rythme peut trancher
// Part de la variance (log) expliquée par la séparation : 0.75 pour une seule population uniforme, proche de 1
// pour deux populations bien distinctes. En dessous, pas de bimodalité : seuil de base (contenu 60 fps...).
constexpr double kMinSeparability = 0.85;
}  // namespace

void DupClassifier::reset() { *this = DupClassifier{}; }

void DupClassifier::update_threshold(float base) {
    base = std::max(base, kFloor);
    if (thr_ < 0.0f) thr_ = base;
    float target = base;

    if (n_metrics_ >= 2 * kMinSplit) {
        std::array<float, kWindow> v{};
        const int n = n_metrics_;
        for (int i = 0; i < n; ++i) v[size_t(i)] = std::log(metrics_[size_t(i)] + 0.05f);
        std::sort(v.begin(), v.begin() + n);

        // Séparation d'Otsu : coupure k (bas = [0, k), haut = [k, n)) qui maximise la variance inter-classes.
        std::array<double, kWindow + 1> pre{};
        for (int i = 0; i < n; ++i) pre[size_t(i + 1)] = pre[size_t(i)] + v[size_t(i)];
        double sq = 0.0;
        for (int i = 0; i < n; ++i) sq += double(v[size_t(i)]) * v[size_t(i)];
        const double mean  = pre[size_t(n)] / n;
        const double total = sq - double(n) * mean * mean;  // n * variance
        int    best_k = -1;
        double best   = -1.0;
        for (int k = kMinSplit; k <= n - kMinSplit; ++k) {
            const double m0 = pre[size_t(k)] / k, m1 = (pre[size_t(n)] - pre[size_t(k)]) / (n - k);
            const double s  = double(k) * double(n - k) / double(n) * (m1 - m0) * (m1 - m0);  // n * var. inter
            if (s > best) {
                best   = s;
                best_k = k;
            }
        }
        if (best_k > 0 && total > 1e-9 && best / total >= kMinSeparability) {
            const float lo_med = std::exp(v[size_t(best_k / 2)]) - 0.05f;
            const float hi_med = std::exp(v[size_t(best_k + (n - best_k) / 2)]) - 0.05f;
            // Bord haut du bruit (dernier du groupe bas) et bord bas du signal (premier du groupe haut).
            const float lo_edge = std::exp(v[size_t(best_k - 1)]) - 0.05f;
            const float hi_edge = std::exp(v[size_t(best_k)]) - 0.05f;
            if (hi_med > kMinRatio * std::max(lo_med, 0.05f) && hi_edge > lo_edge) {
                target = std::sqrt(std::max(lo_edge, 0.05f) * hi_edge);
                target = std::max(target, lo_edge * 1.15f);  // jamais dans le bruit
            }
        }
    }
    target = std::clamp(target, std::max(kFloor, base * 0.25f), base * 4.0f);
    thr_ += (target - thr_) * kSmoothing;
}

void DupClassifier::update_rhythm() {
    period_ = 0;
    if (count_ < uint64_t(kRhythm) || last_new_ == 0) return;
    int best_misses = kMaxMisses + 1;
    for (int p : {2, 3}) {
        // Phase donnée par la dernière nouvelle image : on compare la fenêtre au motif parfait.
        int misses = 0;
        for (int i = 0; i < kRhythm; ++i) {
            const uint64_t idx      = count_ - uint64_t(i);  // index 1-based de la capture
            const bool     expected = (last_new_ >= idx ? last_new_ - idx : idx - last_new_) % uint64_t(p) == 0;
            const bool     got      = calls_[size_t((idx - 1) % kRhythm)] != 0;
            misses += expected != got;
        }
        if (misses < best_misses) {
            best_misses = misses;
            period_     = p;
        }
    }
}

bool DupClassifier::predicted_new() const {
    const uint64_t next = count_ + 1;
    return period_ > 0 && last_new_ > 0 && (next - last_new_) % uint64_t(period_) == 0;
}

DupClassifier::Decision DupClassifier::classify(float metric, const Params& p) {
    Decision d;
    if (!p.adaptive) {
        thr_    = p.base_threshold;
        period_ = 0;
        d.unique    = metric > p.base_threshold;
        d.threshold = p.base_threshold;
    } else {
        metrics_[size_t(pos_)] = std::max(metric, 0.0f);
        pos_       = (pos_ + 1) % kWindow;
        n_metrics_ = std::min(n_metrics_ + 1, kWindow);
        update_threshold(p.base_threshold);

        d.threshold = thr_;
        d.unique    = metric > thr_;
        if (period_ > 0 && metric > thr_ / kAmbiguity && metric < thr_ * kAmbiguity) {
            d.unique    = predicted_new();
            d.predicted = true;
        }
    }

    ++count_;
    calls_[size_t((count_ - 1) % kRhythm)] = d.unique ? 1 : 0;
    if (d.unique) last_new_ = count_;
    if (p.adaptive) update_rhythm();
    d.period = period_;
    return d;
}
