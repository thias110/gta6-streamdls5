#include "audio_sync.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audio_sync {

// --- DriftController -----------------------------------------------------------------------------------

double DriftController::update(double error_ms, double dt_s) {
    if (!std::isfinite(error_ms)) error_ms = 0.0;
    if (!std::isfinite(dt_s) || dt_s < 0.0) dt_s = 0.0;
    if (!init_) {
        filt_ = error_ms;
        init_ = true;
    } else {
        filt_ += (dt_s / (kTauS + dt_s)) * (error_ms - filt_);
    }
    integ_ = std::clamp(integ_ + kKi * filt_ * dt_s, -kMaxPpm, kMaxPpm);
    ppm_   = std::clamp(kKp * filt_ + integ_, -kMaxPpm, kMaxPpm);
    return ppm_;
}

void DriftController::reset() { *this = DriftController{}; }

// --- LinearResampler -----------------------------------------------------------------------------------
//
// Suite virtuelle : s[-1] = prev_, s[0..n_in) = in. La sortie k est prise à la position p = frac_ + k*step
// comptée depuis s[-1] : i = floor(p), t = p - i, valeur = s[i-1] + (s[i] - s[i-1]) * t.

LinearResampler::LinearResampler(uint32_t channels) : ch_(channels ? channels : 1), prev_(ch_, 0.0f) {}

void LinearResampler::reset() {
    frac_ = 0.0;
    std::fill(prev_.begin(), prev_.end(), 0.0f);
}

uint32_t LinearResampler::input_needed(uint32_t n_out, double step) const {
    if (!n_out) return 0;
    const double last = std::floor(frac_ + double(n_out - 1) * step) + 1.0;  // s[i] lu pour la dernière sortie
    const double end  = std::floor(frac_ + double(n_out) * step);            // trames consommées
    return uint32_t(std::max(last, end));
}

uint32_t LinearResampler::output_possible(uint32_t n_in, double step) const {
    if (!n_in || !(step > 0.0)) return 0;
    // Plus grand n tel que input_needed(n) <= n_in ; estimation puis ajustement (arrondis).
    double   est = std::floor((double(n_in) - frac_) / step);
    uint32_t n   = est > 0.0 ? uint32_t(std::min(est + 1.0, 4294967295.0)) : 1u;
    while (n > 0 && input_needed(n, step) > n_in) --n;
    while (input_needed(n + 1, step) <= n_in) ++n;
    return n;
}

uint32_t LinearResampler::process(const float* in, uint32_t n_in, float* out, uint32_t n_out, double step) {
    if (!n_out) return 0;
    for (uint32_t k = 0; k < n_out; ++k) {
        const double   p = frac_ + double(k) * step;
        const uint32_t i = uint32_t(p);
        const float    t = float(p - double(i));
        if (i >= n_in) {  // appel incorrect (entrée trop courte) : on répète la dernière trame plutôt que lire hors limites
            const float* last = n_in ? in + size_t(n_in - 1) * ch_ : prev_.data();
            std::memcpy(out + size_t(k) * ch_, last, ch_ * sizeof(float));
            continue;
        }
        const float* a = i == 0 ? prev_.data() : in + size_t(i - 1) * ch_;
        const float* b = in + size_t(i) * ch_;
        for (uint32_t c = 0; c < ch_; ++c) out[size_t(k) * ch_ + c] = a[c] + (b[c] - a[c]) * t;
    }
    const double   end      = frac_ + double(n_out) * step;
    uint32_t       consumed = uint32_t(std::floor(end));
    consumed                = std::min(consumed, n_in);
    if (consumed >= 1) std::memcpy(prev_.data(), in + size_t(consumed - 1) * ch_, ch_ * sizeof(float));
    frac_ = end - std::floor(end);
    return consumed;
}

// --- Pump ----------------------------------------------------------------------------------------------

Pump::Pump(const PumpConfig& cfg)
    : cfg_(cfg), fifo_(uint32_t(std::max<uint32_t>(cfg.channels, 1) * sizeof(float))), rs_(cfg.channels) {
    if (!cfg_.channels) cfg_.channels = 1;
    if (!cfg_.in_rate) cfg_.in_rate = 48000;
    if (!cfg_.out_rate) cfg_.out_rate = 48000;
}

void Pump::push(const float* frames, uint32_t n) {
    fifo_.push(reinterpret_cast<const uint8_t*>(frames), n);
}

uint32_t Pump::pull(float* out, uint32_t room, uint32_t padding, float delay_ms, double now_s) {
    const double dt = have_now_ ? std::clamp(now_s - last_now_, 0.0, 0.5) : 0.0;
    have_now_       = true;
    last_now_       = now_s;

    const uint32_t ch         = cfg_.channels;
    const double   r          = in_per_out();
    const uint32_t fill_out   = audio_delay::ms_to_frames(cfg_.out_fill_ms, cfg_.out_rate);
    const uint32_t jitter_in  = audio_delay::ms_to_frames(cfg_.jitter_ms, cfg_.in_rate);
    const uint32_t want_delay = audio_delay::ms_to_frames(std::clamp(delay_ms, 0.0f, audio_delay::kMaxMs), cfg_.in_rate);
    const uint32_t want       = padding < fill_out ? std::min(room, fill_out - padding) : 0;

    uint32_t written       = 0;
    auto     write_silence = [&](uint32_t n) {
        if (!n) return;
        std::memset(out + size_t(written) * ch, 0, size_t(n) * ch * sizeof(float));
        written += n;
        silence_ += n;
    };

    if (!started_) {
        // Amorçage : silence dans la sortie jusqu'à ce que la file contienne le retard voulu + la marge.
        delay_in_    = want_delay;
        pending_sil_ = 0;
        if (fifo_.frames() < want_delay + jitter_in) {
            write_silence(want);
            return written;
        }
        started_ = true;
        ctrl_.reset();
        rs_.reset();
    } else if (want_delay != delay_in_) {
        // Retard voulu modifié (--audio-delay, interpolation) : appliqué d'un coup, hors asservissement.
        if (want_delay > delay_in_) {
            pending_sil_ += uint32_t(std::lround(double(want_delay - delay_in_) / r));
        } else {
            uint32_t       d      = delay_in_ - want_delay;
            const uint32_t cancel = std::min(pending_sil_, uint32_t(std::lround(double(d) / r)));
            pending_sil_ -= cancel;
            d -= std::min(d, uint32_t(std::lround(double(cancel) * r)));
            const uint32_t k = std::min(d, fifo_.frames());
            fifo_.drop(k);
            dropped_ += k;
        }
        delay_in_ = want_delay;
    }

    // Latence totale (trames d'entrée) : file + sortie + silence promis, comparée à la cible.
    const double target = double(delay_in_) + double(jitter_in) + double(fill_out) * r;
    double       err     = double(fifo_.frames()) + double(padding + pending_sil_) * r - target;
    const double excess  = double(audio_delay::ms_to_frames(cfg_.max_excess_ms, cfg_.in_rate));
    const double deficit = double(audio_delay::ms_to_frames(cfg_.max_deficit_ms, cfg_.in_rate));
    if (err < -deficit) {  // gros déficit (entrée coupée puis reprise) : silence d'un coup plutôt que 500 ppm
        pending_sil_ += uint32_t(std::lround(-err / r));
        err = 0.0;
        ++reprimes_;
        ctrl_.rebase();
    } else if (err > excess) {  // écart énorme (sortie bloquée, rafale du pilote) : on jette jusqu'à la cible
        const uint32_t k = std::min(fifo_.frames(), uint32_t(err));
        fifo_.drop(k);
        dropped_ += k;
        err -= double(k);
        ctrl_.rebase();  // la mesure lissée repart de la nouvelle erreur ; la dérive estimée (intégrale) reste
    }
    const double ppm  = ctrl_.update(err * 1000.0 / double(cfg_.in_rate), dt);
    const double step = r * (1.0 + ppm * 1e-6);

    const uint32_t s = std::min(pending_sil_, want);
    write_silence(s);
    pending_sil_ -= s;

    const uint32_t ask = want - s;
    const uint32_t n   = std::min(ask, rs_.output_possible(fifo_.frames(), step));
    if (n) {
        const uint32_t need = rs_.input_needed(n, step);
        tmp_.resize(size_t(need) * ch);
        fifo_.peek(reinterpret_cast<uint8_t*>(tmp_.data()), need);
        const uint32_t used = rs_.process(tmp_.data(), need, out + size_t(written) * ch, n, step);
        fifo_.drop(used);
        written += n;
        relayed_ += n;
    }
    // Entrée en retard (n < ask) : on n'invente rien ; si le manque grossit, le déficit ci-dessus ajoute du silence.
    return written;
}

double retry_delay_s(int attempt) {
    if (attempt <= 0) return 0.5;
    return attempt >= 4 ? 5.0 : std::min(5.0, 0.5 * double(1 << attempt));
}

}  // namespace audio_sync
