#pragma once
#include "../common.hpp"

enum class GpuSection : int { Analyze = 0, Upscale, Flow, Compose, Count };

// Timestamps GPU D3D11, relus avec quelques frames de retard (jamais bloquant).
class GpuProfiler {
public:
    void  init(ID3D11Device* dev);
    void  begin_frame(ID3D11DeviceContext* ctx);
    void  end_frame(ID3D11DeviceContext* ctx);
    void  begin(ID3D11DeviceContext* ctx, GpuSection s);
    void  end(ID3D11DeviceContext* ctx, GpuSection s);
    float ms(GpuSection s) const { return ema_[size_t(s)]; }

private:
    static constexpr int kFrames = 5;
    static constexpr int kPairs  = 16;
    struct Pair {
        ComPtr<ID3D11Query> b, e;
        GpuSection          section{};
        bool                ended = false;
    };
    struct Frame {
        ComPtr<ID3D11Query>        disjoint;
        std::array<Pair, kPairs>   pairs;
        int                        used    = 0;
        bool                       pending = false;
    };
    void collect(ID3D11DeviceContext* ctx, Frame& f);

    std::array<Frame, kFrames>                     frames_;
    std::array<int, size_t(GpuSection::Count)>     open_{};
    std::array<float, size_t(GpuSection::Count)>   ema_{};
    int  cur_    = 0;
    bool active_ = false;
    bool ok_     = false;
};
