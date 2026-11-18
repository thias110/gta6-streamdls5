#include "profiler.hpp"

void GpuProfiler::init(ID3D11Device* dev) {
    D3D11_QUERY_DESC dj{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC ts{D3D11_QUERY_TIMESTAMP, 0};
    for (auto& f : frames_) {
        if (FAILED(dev->CreateQuery(&dj, &f.disjoint))) return;
        for (auto& p : f.pairs) {
            if (FAILED(dev->CreateQuery(&ts, &p.b)) || FAILED(dev->CreateQuery(&ts, &p.e))) return;
        }
    }
    open_.fill(-1);
    ok_ = true;
}

void GpuProfiler::collect(ID3D11DeviceContext* ctx, Frame& f) {
    f.pending = false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (ctx->GetData(f.disjoint.Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    if (dj.Disjoint || dj.Frequency == 0) return;

    std::array<double, size_t(GpuSection::Count)> sum{};
    std::array<bool, size_t(GpuSection::Count)>   seen{};
    for (int i = 0; i < f.used; ++i) {
        auto& p = f.pairs[size_t(i)];
        if (!p.ended) continue;
        UINT64 b = 0, e = 0;
        if (ctx->GetData(p.b.Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(p.e.Get(), &e, sizeof(e), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (e < b) continue;
        sum[size_t(p.section)] += double(e - b) * 1000.0 / double(dj.Frequency);
        seen[size_t(p.section)] = true;
    }
    for (size_t s = 0; s < sum.size(); ++s) {
        if (!seen[s]) continue;
        ema_[s] = ema_[s] == 0.0f ? float(sum[s]) : ema_[s] * 0.9f + float(sum[s]) * 0.1f;
    }
}

void GpuProfiler::begin_frame(ID3D11DeviceContext* ctx) {
    if (!ok_ || active_) return;
    Frame& f = frames_[size_t(cur_)];
    if (f.pending) collect(ctx, f);
    f.used = 0;
    open_.fill(-1);
    ctx->Begin(f.disjoint.Get());
    active_ = true;
}

void GpuProfiler::end_frame(ID3D11DeviceContext* ctx) {
    if (!active_) return;
    Frame& f = frames_[size_t(cur_)];
    ctx->End(f.disjoint.Get());
    f.pending = true;
    cur_      = (cur_ + 1) % kFrames;
    active_   = false;
}

void GpuProfiler::begin(ID3D11DeviceContext* ctx, GpuSection s) {
    if (!active_) return;
    Frame& f = frames_[size_t(cur_)];
    if (f.used >= kPairs) return;
    Pair& p   = f.pairs[size_t(f.used)];
    p.section = s;
    p.ended   = false;
    ctx->End(p.b.Get());
    open_[size_t(s)] = f.used++;
}

void GpuProfiler::end(ID3D11DeviceContext* ctx, GpuSection s) {
    if (!active_) return;
    int idx = open_[size_t(s)];
    if (idx < 0) return;
    Pair& p = frames_[size_t(cur_)].pairs[size_t(idx)];
    ctx->End(p.e.Get());
    p.ended          = true;
    open_[size_t(s)] = -1;
}
