#include "motion.hpp"
#include "../gpu/shaders.hpp"

#include <cmath>

namespace {
// Tampon de mesure : [0] max tuile, [1] somme, [2] nb tuiles (cs_tile_diff) ; à partir de [4] les
// statistiques de coupure (cs_cut_stats) : n, sa, sb, saa, sbb, sab[9], histogrammes a puis b.
constexpr UINT kCutFirst  = 4;
constexpr UINT kCutHistA  = kCutFirst + 5 + kCutShifts;  // 18
constexpr UINT kCutHistB  = kCutHistA + kCutBins;        // 50
constexpr UINT kDiffBytes = 512;
static_assert((kCutHistB + kCutBins) * 4 <= kDiffBytes);
}  // namespace

void LumaPyramid::ensure(ID3D11Device* dev, Size2D src, float s) {
    if (src == source && s == scale && !levels.empty()) return;
    levels.clear();
    source = src;
    scale  = s;
    uint32_t w = (src.width + 1) / 2, h = (src.height + 1) / 2;
    if (s < 1.0f) {
        w = std::max(1u, uint32_t(std::lround(double(src.width) * 0.5 * s)));
        h = std::max(1u, uint32_t(std::lround(double(src.height) * 0.5 * s)));
    }
    for (int i = 0; i < kMaxLevels; ++i) {
        GpuTexture t;
        t.ensure(dev, w, h, DXGI_FORMAT_R16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, "luma");
        levels.push_back(std::move(t));
        if (std::min(w, h) / 2 < 24) break;
        w = (w + 1) / 2;
        h = (h + 1) / 2;
    }
}

void MotionEngine::init(D3DContext& d3d) {
    d3d_          = &d3d;
    cs_luma_rgb_  = d3d.make_cs(shaders::kLuma, "cs_luma_from_rgb");
    cs_luma_rgb_scaled_ = d3d.make_cs(shaders::kLuma, "cs_luma_from_rgb_scaled");
    cs_luma_down_ = d3d.make_cs(shaders::kLuma, "cs_luma_down");
    cs_diff_      = d3d.make_cs(shaders::kTileDiff, "cs_tile_diff");
    cs_cut_       = d3d.make_cs(shaders::kCutStats, "cs_cut_stats");
    cs_sobel_     = d3d.make_cs(shaders::kEdges, "cs_edge_sobel");
    cs_dilate_    = d3d.make_cs(shaders::kEdges, "cs_edge_dilate");
    cs_motion_    = d3d.make_cs(shaders::kMotion, "cs_motion_search");
    cs_propagate_ = d3d.make_cs(shaders::kMotion, "cs_motion_propagate");
    cs_median_    = d3d.make_cs(shaders::kMedian, "cs_flow_median");

    auto* dev = d3d.device();
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kDiffBytes;
    bd.Usage     = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format              = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension       = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements  = kDiffBytes / 4;
    ud.Buffer.Flags        = D3D11_BUFFER_UAV_FLAG_RAW;
    D3D11_BUFFER_DESC sd = bd;
    sd.Usage          = D3D11_USAGE_STAGING;
    sd.BindFlags      = 0;
    sd.MiscFlags      = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < 2; ++i) {
        check_hr(dev->CreateBuffer(&bd, nullptr, &diff_buf_[i]), "diff buffer");
        check_hr(dev->CreateUnorderedAccessView(diff_buf_[i].Get(), &ud, &diff_uav_[i]), "diff uav");
        check_hr(dev->CreateBuffer(&sd, nullptr, &diff_staging_[i]), "diff staging");
    }
}

void MotionEngine::build_pyramid(LumaPyramid& pyr, ID3D11ShaderResourceView* rgb, Size2D src, float scale) {
    auto* ctx = d3d_->ctx();
    scale = std::clamp(scale, 0.5f, 1.0f);
    pyr.ensure(d3d_->device(), src, scale);
    if (scale < 1.0f) d3d_->bind_samplers_cs();
    struct {
        uint32_t dw, dh, sw, sh;
    } cb{};

    for (size_t i = 0; i < pyr.levels.size(); ++i) {
        const GpuTexture& dst = pyr.levels[i];
        ID3D11ShaderResourceView* srvs[2] = {nullptr, nullptr};
        if (i == 0) {
            cb      = {dst.width, dst.height, src.width, src.height};
            srvs[0] = rgb;
            ctx->CSSetShader(scale < 1.0f ? cs_luma_rgb_scaled_.Get() : cs_luma_rgb_.Get(), nullptr, 0);
        } else {
            const GpuTexture& s = pyr.levels[i - 1];
            cb      = {dst.width, dst.height, s.width, s.height};
            srvs[1] = s.srv.Get();
            ctx->CSSetShader(cs_luma_down_.Get(), nullptr, 0);
        }
        ID3D11Buffer* b = d3d_->constants(cb);
        ctx->CSSetConstantBuffers(0, 1, &b);
        ctx->CSSetShaderResources(0, 2, srvs);
        ID3D11UnorderedAccessView* uav = dst.uav.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->Dispatch(div_up(dst.width, 8), div_up(dst.height, 8), 1);
        d3d_->unbind_cs();
    }
}

void MotionEngine::build_edges(const LumaPyramid& pyr, GpuTexture& out) {
    if (!pyr.valid()) return;
    auto* ctx = d3d_->ctx();
    auto* dev = d3d_->device();
    const GpuTexture& l0 = pyr.levels[0];
    const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    edge_tmp_.ensure(dev, l0.width, l0.height, DXGI_FORMAT_R16_FLOAT, bind, "edges tmp");
    out.ensure(dev, l0.width, l0.height, DXGI_FORMAT_R16_FLOAT, bind, "edges");

    // Marche de luminance de ~16 à ~32 niveaux sur 255 (Sobel L1 = 4 x marche).
    struct {
        uint32_t w, h;
        float    lo, hi;
    } cb{l0.width, l0.height, 4.0f * 16.0f / 255.0f, 4.0f * 32.0f / 255.0f};
    ID3D11Buffer* b = d3d_->constants(cb);
    const std::pair<ID3D11ComputeShader*, std::pair<const GpuTexture*, GpuTexture*>> passes[] = {
        {cs_sobel_.Get(), {&l0, &edge_tmp_}}, {cs_dilate_.Get(), {&edge_tmp_, &out}}};
    for (const auto& [cs, io] : passes) {
        ctx->CSSetConstantBuffers(0, 1, &b);
        ID3D11ShaderResourceView* srv = io.first->srv.Get();
        ctx->CSSetShaderResources(0, 1, &srv);
        ID3D11UnorderedAccessView* uav = io.second->uav.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
        ctx->Dispatch(div_up(l0.width, 8), div_up(l0.height, 8), 1);
        d3d_->unbind_cs();
    }
}

void MotionEngine::begin_measure(const LumaPyramid& a, const LumaPyramid& b) {
    if (!a.compatible(b)) {
        pending_           = -1;
        pending_immediate_ = true;
        immediate_         = {255.0f, 255.0f, {}};
        return;
    }
    pending_immediate_ = false;
    const int buf = write_;
    write_ ^= 1;

    auto* ctx = d3d_->ctx();
    // ~quart de résolution de la capture : niveau 1, ou niveau 0 si la pyramide est déjà réduite.
    const size_t lvl = std::min<size_t>(a.scale < 0.75f ? 0 : 1, a.levels.size() - 1);
    const GpuTexture& la = a.levels[lvl];
    const GpuTexture& lb = b.levels[lvl];

    const UINT zero[4] = {};
    ctx->ClearUnorderedAccessViewUint(diff_uav_[buf].Get(), zero);
    struct {
        uint32_t w, h, p0, p1;
    } cb{la.width, la.height, 0, 0};
    ID3D11Buffer* cbuf = d3d_->constants(cb);
    ctx->CSSetConstantBuffers(0, 1, &cbuf);
    ID3D11ShaderResourceView* srvs[2] = {la.srv.Get(), lb.srv.Get()};
    ctx->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uav = diff_uav_[buf].Get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetShader(cs_diff_.Get(), nullptr, 0);
    ctx->Dispatch(div_up(la.width, 16), div_up(la.height, 16), 1);
    d3d_->unbind_cs();

    // Statistiques de coupure sur le niveau le plus grossier (même taille dans a et b).
    const GpuTexture& ca = a.levels.back();
    const GpuTexture& cb_ = b.levels.back();
    struct {
        uint32_t w, h, p0, p1;
    } ccb{ca.width, ca.height, 0, 0};
    ID3D11Buffer* ccbuf = d3d_->constants(ccb);
    ctx->CSSetConstantBuffers(0, 1, &ccbuf);
    ID3D11ShaderResourceView* csrvs[2] = {ca.srv.Get(), cb_.srv.Get()};
    ctx->CSSetShaderResources(0, 2, csrvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetShader(cs_cut_.Get(), nullptr, 0);
    ctx->Dispatch(1, 1, 1);
    d3d_->unbind_cs();

    ctx->CopyResource(diff_staging_[buf].Get(), diff_buf_[buf].Get());
    pending_ = buf;
}

bool MotionEngine::read_measure(DiffResult& out, bool wait) {
    if (pending_immediate_) {
        out                = immediate_;
        pending_immediate_ = false;
        return true;
    }
    if (pending_ < 0) return false;
    auto* ctx = d3d_->ctx();
    ID3D11Buffer* st = diff_staging_[pending_].Get();
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx->Map(st, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (!wait) return false;
        ++blocking_reads_;
        hr = ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    }
    check_hr(hr, "Map diff");
    const auto* v = static_cast<const uint32_t*>(m.pData);
    DiffResult r;
    r.max_tile = float(v[0]) / 1000.0f;
    r.mean     = v[2] ? float(v[1]) / 1000.0f / float(v[2]) : 0.0f;
    float f[5 + kCutShifts];
    std::memcpy(f, v + kCutFirst, sizeof(f));
    r.cut.n   = f[0];
    r.cut.sa  = f[1];
    r.cut.sb  = f[2];
    r.cut.saa = f[3];
    r.cut.sbb = f[4];
    std::memcpy(r.cut.sab, f + 5, sizeof(r.cut.sab));
    std::memcpy(r.cut.ha, v + kCutHistA, sizeof(r.cut.ha));
    std::memcpy(r.cut.hb, v + kCutHistB, sizeof(r.cut.hb));
    ctx->Unmap(st, 0);
    pending_ = -1;
    out      = r;
    return true;
}

DiffResult MotionEngine::measure_difference(const LumaPyramid& a, const LumaPyramid& b) {
    begin_measure(a, b);
    DiffResult r{255.0f, 255.0f, {}};
    read_measure(r, true);
    return r;
}

void MotionEngine::estimate_flow(const LumaPyramid& x, const LumaPyramid& y, ID3D11ShaderResourceView* temporal,
                                 float temporal_sign, GpuTexture& out) {
    auto* ctx = d3d_->ctx();
    auto* dev = d3d_->device();
    const int top = int(x.levels.size()) - 1;
    if (level_flow_.size() < x.levels.size()) {
        level_flow_.resize(x.levels.size());
        level_tmp_.resize(x.levels.size());
    }
    d3d_->bind_samplers_cs();

    struct MotionCB {
        int32_t  size[2];
        int32_t  grid[2];
        float    inv[2];
        int32_t  radius;
        uint32_t flags;
        float    temporal_sign, lambda, zero_bias, pad;
        float    coarse_scale[2];
        float    temporal_scale[2];
    };
    static_assert(sizeof(MotionCB) == 64);
    // Le flow temporel est le niveau 0 de la paire précédente (même taille de capture).
    const FlowScale ts = flow_grid_scale(x.levels[0].width, x.levels[0].height);

    for (int L = top; L >= 0; --L) {
        const GpuTexture& lx = x.levels[size_t(L)];
        const GpuTexture& ly = y.levels[size_t(L)];
        const uint32_t gw = div_up(lx.width, kFlowBlock), gh = div_up(lx.height, kFlowBlock);
        GpuTexture& dst = level_flow_[size_t(L)];
        dst.ensure(dev, gw, gh, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                   "flow level");

        MotionCB cb{};
        cb.size[0] = int32_t(lx.width);
        cb.size[1] = int32_t(lx.height);
        cb.grid[0] = int32_t(gw);
        cb.grid[1] = int32_t(gh);
        cb.inv[0]  = 1.0f / float(lx.width);
        cb.inv[1]  = 1.0f / float(lx.height);
        cb.radius  = L == top ? 6 : (L == top - 1 ? 3 : 2);
        cb.flags   = (L < top ? 1u : 0u) | (temporal ? 2u : 0u);
        cb.temporal_sign = temporal_sign;
        cb.lambda        = L == top ? 0.0f : 0.0012f;
        cb.zero_bias     = 0.0025f;
        if (L < top) {
            const GpuTexture& lc = x.levels[size_t(L + 1)];
            const FlowScale   cs = flow_grid_scale(lc.width, lc.height);
            cb.coarse_scale[0] = cs.x;
            cb.coarse_scale[1] = cs.y;
        } else {
            cb.coarse_scale[0] = cb.coarse_scale[1] = 1.0f;
        }
        cb.temporal_scale[0] = ts.x;
        cb.temporal_scale[1] = ts.y;

        ID3D11Buffer* b = d3d_->constants(cb);
        ctx->CSSetConstantBuffers(0, 1, &b);
        ID3D11ShaderResourceView* srvs[4] = {lx.srv.Get(), ly.srv.Get(),
                                             L < top ? level_flow_[size_t(L + 1)].srv.Get() : nullptr, temporal};
        ctx->CSSetShaderResources(0, 4, srvs);
        ID3D11UnorderedAccessView* uav = dst.uav.Get();
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        ctx->CSSetShader(cs_motion_.Get(), nullptr, 0);
        ctx->Dispatch(div_up(gw, 8), div_up(gh, 8), 1);
        d3d_->unbind_cs();

        // Propagation entre voisins (ping-pong), sous-pixel à la dernière passe du niveau 0.
        GpuTexture& tmp = level_tmp_[size_t(L)];
        tmp.ensure(dev, gw, gh, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                   "flow tmp");
        const int iterations = L == 0 ? 3 : 2;
        for (int it = 0; it < iterations; ++it) {
            GpuTexture& in  = level_flow_[size_t(L)];
            MotionCB    pcb = cb;
            pcb.lambda      = 0.0f;
            pcb.flags       = (L == 0 && it == iterations - 1) ? 4u : 0u;
            ID3D11Buffer* pb = d3d_->constants(pcb);
            ctx->CSSetConstantBuffers(0, 1, &pb);
            ID3D11ShaderResourceView* psrv[3] = {lx.srv.Get(), ly.srv.Get(), in.srv.Get()};
            ctx->CSSetShaderResources(0, 3, psrv);
            ID3D11UnorderedAccessView* puav = tmp.uav.Get();
            ctx->CSSetUnorderedAccessViews(0, 1, &puav, nullptr);
            ctx->CSSetShader(cs_propagate_.Get(), nullptr, 0);
            ctx->Dispatch(div_up(gw, 8), div_up(gh, 8), 1);
            d3d_->unbind_cs();
            std::swap(level_flow_[size_t(L)], tmp);
        }
    }

    const GpuTexture& raw = level_flow_[0];
    out.ensure(dev, raw.width, raw.height, DXGI_FORMAT_R32G32_FLOAT,
               D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, "flow");
    struct {
        int32_t w, h, p0, p1;
    } mcb{int32_t(raw.width), int32_t(raw.height), 0, 0};
    ID3D11Buffer* b = d3d_->constants(mcb);
    ctx->CSSetConstantBuffers(0, 1, &b);
    ID3D11ShaderResourceView* srv = raw.srv.Get();
    ctx->CSSetShaderResources(0, 1, &srv);
    ID3D11UnorderedAccessView* uav = out.uav.Get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->CSSetShader(cs_median_.Get(), nullptr, 0);
    ctx->Dispatch(div_up(raw.width, 8), div_up(raw.height, 8), 1);
    d3d_->unbind_cs();
}
