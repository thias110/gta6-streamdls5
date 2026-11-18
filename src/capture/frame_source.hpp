#pragma once
#include "../gpu/d3d_context.hpp"

struct SourceFrame {
    const GpuTexture* texture = nullptr;  // BGRA, SRV + RT, valide jusqu'au prochain poll()
    double            time    = 0.0;      // secondes, horloge now_seconds()
};

class FrameSource {
public:
    virtual ~FrameSource() = default;
    // Renvoie la frame la plus récente si une nouvelle est arrivée depuis le dernier appel.
    virtual bool        poll(SourceFrame& out) = 0;
    virtual HANDLE      wait_handle() const { return nullptr; }  // signalé à l'arrivée d'une frame
    virtual double      next_due() const { return 0.0; }        // sources cadencées sans event
    virtual bool        alive() const { return true; }
    virtual std::string name() const = 0;
};
