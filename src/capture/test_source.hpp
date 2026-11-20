#pragma once
#include "../pipeline/compositor.hpp"
#include "frame_source.hpp"

#include <random>

// Mire analytique rendue par shader (vérité terrain exacte à n'importe quel instant).
class TestPattern {
public:
    void init(D3DContext& d3d, Compositor& comp);
    // speed : multiplicateur des vitesses ; noise : bruit façon compression ; seed : graine du bruit
    void render(const GpuTexture& dst, double t, float noise, float seed, float speed);

private:
    D3DContext*               d3d_  = nullptr;
    Compositor*               comp_ = nullptr;
    ComPtr<ID3D11PixelShader> ps_;
};

// Profil RemotePlay (défaut) : simule un flux Remote Play, jeu à 30 fps envoyé à 60 fps (une frame sur deux
// doublée, avec un bruit différent comme après réencodage) et une gigue réseau de +-2 ms.
// Profil Cable (--source capture --test-pattern) : simule une carte de capture HDMI, jeu à 30 fps dans un signal
// 60 Hz, répétitions identiques au pixel (pas de réencodage), horodatages réguliers (déterministe).
class TestSource final : public FrameSource {
public:
    enum class Profile { RemotePlay, Cable };
    TestSource(D3DContext& d3d, Compositor& comp, Profile profile = Profile::RemotePlay);
    bool        poll(SourceFrame& out) override;
    double      next_due() const override { return next_; }
    std::string name() const override {
        return profile_ == Profile::Cable ? "Mire de test, profil câble (jeu 30 fps dans un signal HDMI 60 Hz)"
                                          : "Mire de test (jeu 30 fps dans un flux 60 fps)";
    }

private:
    D3DContext& d3d_;
    Profile     profile_;
    TestPattern pattern_;
    GpuTexture  tex_;
    double      start_ = 0.0, next_ = 0.0;
    uint64_t    n_ = 0;
    std::mt19937 rng_{42};
};
