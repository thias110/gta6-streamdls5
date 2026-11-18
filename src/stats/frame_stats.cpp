#include "frame_stats.hpp"

void FrameStatsCollector::prune(std::deque<double>& d, double t) {
    while (!d.empty() && d.front() < t - 1.0) d.pop_front();
}

void FrameStatsCollector::on_capture(double t, bool unique, float metric) {
    cap_.push_back(t);
    prune(cap_, t);
    ++n_cap_;
    metric_[size_t(metric_pos_)] = std::max(metric, 0.0f);
    metric_pos_ = (metric_pos_ + 1) % kHistory;
    if (!unique) return;

    uniq_.push_back(t);
    prune(uniq_, t);
    ++n_uniq_;
    if (last_unique_ > 0.0) {
        interval_[size_t(interval_pos_)] = float((t - last_unique_) * 1000.0);
        interval_pos_ = (interval_pos_ + 1) % kHistory;
    }
    last_unique_ = t;
}

void FrameStatsCollector::on_present(double t, bool generated) {
    pres_.push_back(t);
    prune(pres_, t);
    ++n_pres_;
    if (generated) {
        gen_.push_back(t);
        ++n_gen_;
    }
    prune(gen_, t);
}

FrameStatsCollector::Snapshot FrameStatsCollector::snapshot(double now) const {
    auto count = [now](const std::deque<double>& d) {
        return float(std::count_if(d.begin(), d.end(), [now](double t) { return t >= now - 1.0; }));
    };
    Snapshot s;
    s.captured_fps  = count(cap_);
    s.unique_fps    = count(uniq_);
    s.present_fps   = count(pres_);
    float gen       = count(gen_);
    s.dup_pct       = s.captured_fps > 0 ? 100.0f * (1.0f - s.unique_fps / s.captured_fps) : 0.0f;
    s.generated_pct = s.present_fps > 0 ? 100.0f * gen / s.present_fps : 0.0f;
    s.captured  = n_cap_;
    s.unique    = n_uniq_;
    s.presented = n_pres_;
    s.generated = n_gen_;
    return s;
}
