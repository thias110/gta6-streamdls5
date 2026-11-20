#include "enhancer.hpp"
#include "../gpu/shaders.hpp"

void Enhancer::init(D3DContext& d3d, Compositor& comp) {
    d3d_        = &d3d;
    comp_       = &comp;
    ps_denoise_ = d3d.make_ps(shaders::kDenoise, "ps_denoise");
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
