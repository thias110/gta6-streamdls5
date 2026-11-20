#include "compositor.hpp"
#include "../gpu/shaders.hpp"

void Compositor::init(D3DContext& d3d) {
    d3d_   = &d3d;
    blit_  = d3d.make_ps(shaders::kBlit, "ps_blit");
    synth_ = d3d.make_ps(shaders::kSynth, "ps_synth");
    viz_   = d3d.make_ps(shaders::kFlowViz, "ps_flow_viz");

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode        = D3D11_FILL_SOLID;
    rd.CullMode        = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    check_hr(d3d.device()->CreateRasterizerState(&rd, &rs_plain_), "rasterizer");
    rd.ScissorEnable = TRUE;
    check_hr(d3d.device()->CreateRasterizerState(&rd, &rs_scissor_), "rasterizer scissor");
}

void Compositor::begin(ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp) {
    auto* ctx = d3d_->ctx();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(rs_plain_.Get());
    d3d_->bind_samplers_ps();
}

void Compositor::end() {
    auto* ctx = d3d_->ctx();
    ID3D11ShaderResourceView* nulls[4]{};
    ctx->PSSetShaderResources(0, 4, nulls);
    ctx->RSSetState(rs_plain_.Get());
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void Compositor::scissor(const D3D11_RECT* r) {
    auto* ctx = d3d_->ctx();
    if (r) {
        ctx->RSSetScissorRects(1, r);
        ctx->RSSetState(rs_scissor_.Get());
    } else {
        ctx->RSSetState(rs_plain_.Get());
    }
}

void Compositor::draw(ID3D11PixelShader* ps, std::initializer_list<ID3D11ShaderResourceView*> srvs, ID3D11Buffer* cb) {
    auto* ctx = d3d_->ctx();
    ID3D11ShaderResourceView* arr[4]{};
    UINT n = 0;
    for (auto* s : srvs) {
        if (n < 4) arr[n++] = s;
    }
    ctx->PSSetShaderResources(0, 4, arr);
    if (cb) ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShader(ps, nullptr, 0);
    d3d_->draw_fullscreen();
}

void Compositor::blit(ID3D11ShaderResourceView* src) { draw(blit_.Get(), {src}); }

void Compositor::synth(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, ID3D11ShaderResourceView* flow_ab,
                       ID3D11ShaderResourceView* flow_ba, float t, const SynthParams& p, FlowScale fs) {
    struct {
        float t, slo, shi, elo, ehi, fsx, fsy, pad;
    } cb{t, p.static_lo, p.static_hi, p.err_lo, p.err_hi, fs.x, fs.y, 0.0f};
    static_assert(sizeof(cb) == 32);
    draw(synth_.Get(), {a, b, flow_ab, flow_ba}, d3d_->constants(cb));
}

void Compositor::flow_viz(ID3D11ShaderResourceView* img, ID3D11ShaderResourceView* flow, float scale,
                          FlowScale fs) {
    struct {
        float scale, pad, fsx, fsy;
    } cb{scale, 0.0f, fs.x, fs.y};
    draw(viz_.Get(), {img, flow}, d3d_->constants(cb));
}
