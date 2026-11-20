#pragma once
#include "../gpu/d3d_context.hpp"

// Métadonnées facultatives d'une frame, fournies par une source qui les connaît (frame tap). Les valeurs par
// défaut sont neutres : WGC et la mire ne les remplissent pas.
struct SourceFrameMeta {
    bool     valid       = false;  // la source renseigne les champs ci-dessous
    bool     repeat      = false;  // l'émetteur dit : répétition de la frame précédente
    bool     fresh       = false;  // l'émetteur dit : nouveau contenu
    bool     truncated   = false;  // frame incomplète (ne tenait pas dans le slot)
    int32_t  frames_lost = 0;      // frames perdues juste avant celle-ci (pertes réseau)
    bool     recovered   = false;  // première frame qui répare l'image après une perte (keyframe)
    bool     new_session = false;  // première frame lue d'un nouveau flux de l'émetteur
    uint32_t session     = 0;
    uint64_t seq         = 0;      // numéro de la frame dans la session de l'émetteur
};

struct SourceFrame {
    const GpuTexture* texture = nullptr;  // BGRA, SRV + RT, valide jusqu'au prochain poll()
    double            time    = 0.0;      // secondes, horloge now_seconds()
    SourceFrameMeta   meta;               // neutre par défaut
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
