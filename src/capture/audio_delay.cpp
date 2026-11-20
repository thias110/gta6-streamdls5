#include "audio_delay.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace audio_delay {

bool parse(std::wstring_view text, Setting& out, std::string* error) {
    std::wstring v(text);
    for (auto& c : v) c = wchar_t(std::towlower(wint_t(c)));
    if (v == L"auto") {
        out = {Mode::Auto, 0.0f};
        return true;
    }
    if (v == L"off" || v == L"non") {
        out = {Mode::Off, 0.0f};
        return true;
    }
    if (v.size() > 2 && v.compare(v.size() - 2, 2, L"ms") == 0) v.resize(v.size() - 2);
    wchar_t*     end = nullptr;
    const double ms  = v.empty() ? -1.0 : std::wcstod(v.c_str(), &end);
    if (v.empty() || !end || *end != L'\0' || !(ms >= 0.0) || ms > double(kMaxMs)) {
        if (error) *error = "--audio-delay attend auto, off ou un retard en ms de 0 à 250";
        return false;
    }
    out = {Mode::Fixed, float(ms)};
    return true;
}

std::string describe(const Setting& s) {
    if (s.mode == Mode::Auto) return "auto";
    if (s.mode == Mode::Off) return "off";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f ms", double(s.ms));
    return buf;
}

float interpolation_extra_ms(bool frame_gen, float delay_ms, float arrival_ms) {
    if (!frame_gen || !std::isfinite(delay_ms)) return 0.0f;
    const float a = std::isfinite(arrival_ms) ? std::max(arrival_ms, 0.0f) : 0.0f;
    return std::clamp(delay_ms - a, 0.0f, kMaxMs);
}

float target_ms(const Setting& s, bool frame_gen, float delay_ms, float arrival_ms) {
    switch (s.mode) {
    case Mode::Off: return 0.0f;
    case Mode::Fixed: return std::clamp(s.ms, 0.0f, kMaxMs);
    default: return interpolation_extra_ms(frame_gen, delay_ms, arrival_ms);
    }
}

float Tracker::update(float target, double now_s) {
    target = std::clamp(std::isfinite(target) ? target : 0.0f, 0.0f, kMaxMs);
    if (!init_) {
        init_        = true;
        cur_         = target;
        last_change_ = now_s;
        return cur_;
    }
    const bool to_zero = target == 0.0f && cur_ != 0.0f;
    if (to_zero || (std::abs(target - cur_) >= kStepMs && now_s - last_change_ >= kHoldS)) {
        cur_         = target;
        last_change_ = now_s;
    }
    return cur_;
}

uint32_t ms_to_frames(float ms, uint32_t sample_rate) {
    if (!(ms > 0.0f)) return 0;
    return uint32_t(std::lround(double(ms) * double(sample_rate) / 1000.0));
}

Plan plan(uint32_t fifo, uint32_t target, uint32_t room, uint32_t padding, uint32_t max_excess, uint32_t max_padding) {
    Plan p;
    if (uint64_t(fifo) > uint64_t(target) + max_excess) p.drop = fifo - target;
    const uint32_t avail = fifo - p.drop;
    const uint32_t ready = avail > target ? avail - target : 0;
    if (padding > max_padding) {
        p.drop += ready;  // la sortie a pris du retard : on rattrape
    } else {
        p.write = std::min(ready, room);
    }
    return p;
}

void FrameFifo::reset(uint32_t block_bytes) {
    block_ = block_bytes ? block_bytes : 1;
    buf_.clear();
    head_ = size_ = 0;
}

void FrameFifo::grow(size_t need) {
    if (need <= buf_.size()) return;
    size_t cap = std::max<size_t>(buf_.size() * 2, 4096);
    while (cap < need) cap *= 2;
    std::vector<uint8_t> n(cap);
    // Copie linéaire du contenu (en deux morceaux si l'anneau boucle).
    const size_t first = std::min(size_, buf_.size() - head_);
    if (first) std::memcpy(n.data(), buf_.data() + head_, first);
    if (size_ > first) std::memcpy(n.data() + first, buf_.data(), size_ - first);
    buf_.swap(n);
    head_ = 0;
}

void FrameFifo::push(const uint8_t* data, uint32_t frames) {
    const size_t bytes = size_t(frames) * block_;
    if (!bytes) return;
    grow(size_ + bytes);
    size_t tail = (head_ + size_) % buf_.size();
    size_t done = 0;
    while (done < bytes) {
        const size_t chunk = std::min(bytes - done, buf_.size() - tail);
        if (data) std::memcpy(buf_.data() + tail, data + done, chunk);
        else std::memset(buf_.data() + tail, 0, chunk);
        done += chunk;
        tail = (tail + chunk) % buf_.size();
    }
    size_ += bytes;
}

uint32_t FrameFifo::pop(uint8_t* dst, uint32_t frames) {
    const uint32_t n     = std::min(frames, this->frames());
    const size_t   bytes = size_t(n) * block_;
    size_t         done  = 0;
    while (done < bytes) {
        const size_t chunk = std::min(bytes - done, buf_.size() - head_);
        if (dst) std::memcpy(dst + done, buf_.data() + head_, chunk);
        done += chunk;
        head_ = (head_ + chunk) % buf_.size();
    }
    size_ -= bytes;
    if (size_ == 0) head_ = 0;
    return n;
}

void FrameFifo::drop(uint32_t frames) { pop(nullptr, frames); }

}  // namespace audio_delay
