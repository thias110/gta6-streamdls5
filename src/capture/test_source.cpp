#include "test_source.hpp"
#include "../gpu/shaders.hpp"

#include <cmath>

void TestPattern::init(D3DContext& d3d, Compositor& comp) {
    d3d_  = &d3d;
    comp_ = &comp;
    ps_   = d3d.make_ps(shaders::kTestPattern, "ps_test_pattern");
}

void TestPattern::render(const GpuTexture& dst, double t, float noise, float seed, float speed) {
    struct {
        float time, noise, seed, speed;
    } cb{float(t), noise, seed, speed};
    const D3D11_VIEWPORT vp{0.0f, 0.0f, float(dst.width), float(dst.height), 0.0f, 1.0f};
    comp_->begin(dst.rtv.Get(), vp);
    comp_->draw(ps_.Get(), {}, d3d_->constants(cb));
    comp_->end();
}

TestSource::TestSource(D3DContext& d3d, Compositor& comp) : d3d_(d3d) {
    pattern_.init(d3d, comp);
    tex_.ensure(d3d.device(), 1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, "test source");
    start_ = next_ = now_seconds();
}

bool TestSource::poll(SourceFrame& out) {
    const double now = now_seconds();
    if (now < next_) return false;

    // Instant de jeu quantifié à 30 fps : la frame 2k+1 répète la frame 2k.
    const double game_time = std::floor(double(n_) / 2.0) / 30.0;
    pattern_.render(tex_, game_time, 0.012f, float(n_ % 1000), 1.0f);
    out.texture = &tex_;
    out.time    = now;

    ++n_;
    std::uniform_real_distribution<double> jitter(-0.002, 0.002);
    next_ = std::max(now, start_ + double(n_) / 60.0 + jitter(rng_));
    return true;
}
