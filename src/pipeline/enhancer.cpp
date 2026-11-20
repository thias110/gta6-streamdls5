#include "enhancer.hpp"
#include "../gpu/shaders.hpp"

void Enhancer::init(D3DContext& d3d, Compositor& comp) {
    d3d_        = &d3d;
    comp_       = &comp;
    ps_denoise_ = d3d.make_ps(shaders::kDenoise, "ps_denoise");
    ps_finish_  = d3d.make_ps(shaders::kFinish, "ps_finish");
}

const GpuTexture& Enhancer::denoise(const GpuTexture& src, float strength) {
    denoised_.ensure(d3d_->device(), src.width, src.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "enhance denoise");
    const enhance::DenoiseCoeffs k = enhance::denoise_coeffs(strength);
    struct {
        float inv_2ss2, inv_2sr2, mix, pad;
    } cb{1.0f / (2.0f * k.sigma_s * k.sigma_s), 1.0f / (2.0f * k.sigma_r * k.sigma_r), k.mix, 0.0f};
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(src.width), float(src.height), 0.0f, 1.0f};
    comp_->begin(denoised_.rtv.Get(), vp);
    comp_->draw(ps_denoise_.Get(), {src.srv.Get()}, d3d_->constants(cb));
    comp_->end();
    return denoised_;
}

const GpuTexture& Enhancer::finish(const GpuTexture& up, const enhance::Params& p, const FinishInputs& in) {
    finished_.ensure(d3d_->device(), up.width, up.height, DXGI_FORMAT_B8G8R8A8_UNORM,
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "enhance finish");
    const bool pyr  = in.cur && in.cur->valid();
    const bool prev = pyr && in.prev && in.prev->compatible(*in.cur);
    ID3D11ShaderResourceView* l0   = pyr ? in.cur->levels[0].srv.Get() : nullptr;
    ID3D11ShaderResourceView* lp   = prev ? in.prev->levels[0].srv.Get() : nullptr;
    ID3D11ShaderResourceView* base = pyr ? in.cur->levels[std::min<size_t>(1, in.cur->levels.size() - 1)].srv.Get()
                                         : nullptr;
    struct {
        float sharpen, clarity, tone, sat;
        float dither, hud, has_prev, has_edge;
        float has_pyr, pad[3];
    } cb{p.sharpen, p.clarity, p.tone, p.saturation,
         p.dither ? 1.0f : 0.0f, in.hud_protect ? 1.0f : 0.0f, prev ? 1.0f : 0.0f, in.edges ? 1.0f : 0.0f,
         pyr ? 1.0f : 0.0f, {}};
    static_assert(sizeof(cb) == 48);
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(up.width), float(up.height), 0.0f, 1.0f};
    comp_->begin(finished_.rtv.Get(), vp);
    comp_->draw(ps_finish_.Get(), {up.srv.Get(), l0, lp, base, in.edges}, d3d_->constants(cb));
    comp_->end();
    return finished_;
}
