#pragma once
#include "../common.hpp"

#include <deque>

// Compteurs de cadence sur une fenêtre glissante d'une seconde + historiques pour les graphes.
class FrameStatsCollector {
public:
    static constexpr int kHistory = 180;

    struct Snapshot {
        float    captured_fps  = 0;
        float    unique_fps    = 0;
        float    present_fps   = 0;
        float    dup_pct       = 0;
        float    generated_pct = 0;
        uint64_t captured = 0, unique = 0, presented = 0, generated = 0;
    };

    void     on_capture(double t, bool unique, float metric);
    void     on_present(double t, bool generated);
    Snapshot snapshot(double now) const;

    const float* metric_history() const { return metric_.data(); }
    const float* interval_history() const { return interval_.data(); }
    int          metric_offset() const { return metric_pos_; }
    int          interval_offset() const { return interval_pos_; }

private:
    static void prune(std::deque<double>& d, double t);

    std::deque<double> cap_, uniq_, pres_, gen_;
    std::array<float, kHistory> metric_{};
    std::array<float, kHistory> interval_{};
    int      metric_pos_ = 0, interval_pos_ = 0;
    double   last_unique_ = 0.0;
    uint64_t n_cap_ = 0, n_uniq_ = 0, n_pres_ = 0, n_gen_ = 0;
};
