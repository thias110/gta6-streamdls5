#pragma once
#include "../gpu/d3d_context.hpp"

// Backend OPTIONNEL : NVIDIA Video Effects SDK (Maxine VFX, « nvvfx »).
//  - VFG (Video Frame Generation) : image intermédiaire entre deux images, à un instant t dans (0, 1).
//  - VSR (Video Super Resolution) : upscale IA.
//
// Compilé seulement avec l'option CMake RPENHANCE_NVVFX=ON (sinon toutes les méthodes renvoient false
// et le pipeline garde le flow maison et le VSR du VideoProcessor). Même compilé, le SDK est chargé
// dynamiquement (LoadLibrary) au premier usage : l'exe démarre et fonctionne sans le SDK, sans GPU
// Ada/Blackwell, sans DLL ; chaque échec est journalisé une fois et l'appelant se replie.
//
// Ce fichier ne dépend d'aucun en-tête NVIDIA : seule l'implémentation (nvvfx_backend.cpp) inclut les
// en-têtes du SDK installé localement, jamais copiés dans le dépôt.
//
// Interop D3D11 <-> CUDA (détails dans nvvfx_backend.cpp) : chaque texture D3D11 est enregistrée une
// fois (NvCVImage_InitFromD3DTexture), puis, à chaque usage, mappée sur le flux CUDA, copiée GPU->GPU
// (NvCVImage_Transfer) vers un tampon CUDA linéaire, et démappée. Aucun aller-retour par le CPU, mais
// une copie GPU par entrée/sortie : les filtres n'acceptent que de la mémoire CUDA linéaire, pas un
// tableau CUDA issu d'une texture. Tout est appelé depuis le thread GPU unique de l'application.

struct NvvfxOptions {
    std::wstring sdk_dir;            // racine du VFX SDK Core ; vide = recherche automatique
    std::wstring model_dir;          // NVVFX_MODEL_DIRECTORY ; vide = ne pas le fixer (défaut du SDK)
    uint32_t     vfg_mode = 1;       // VFG : 0 Low, 1 Medium, 2 High
    uint32_t     vsr_mode = 3;       // VSR : 0..4 (VSR_Bicubic..VSR_Ultra), 3 = VSR_High
    float        vsr_strength = -1;  // VSR : [0, 1] ; < 0 = défaut du SDK

    bool operator==(const NvvfxOptions&) const = default;
};

class NvvfxBackend {
public:
    NvvfxBackend();
    ~NvvfxBackend();
    NvvfxBackend(const NvvfxBackend&)            = delete;
    NvvfxBackend& operator=(const NvvfxBackend&) = delete;

    // true si l'exe a été compilé avec RPENHANCE_NVVFX=ON (ne dit rien de la présence du SDK).
    static bool compiled_in();

    // À appeler avant le premier usage. Un changement après chargement recrée les filtres.
    void set_options(const NvvfxOptions& o);

    // VSR : src (BGRA8) -> dst (BGRA8, plus grand que src). false = indisponible, l'appelant se replie.
    bool upscale(D3DContext& d3d, const GpuTexture& src, const GpuTexture& dst);

    // VFG : image à l'instant t (0 = a, 1 = b) écrite dans out. a, b, out : BGRA8, même taille.
    // a_id / b_id identifient le contenu des textures : une texture déjà copiée vers CUDA avec le même
    // identifiant n'est pas recopiée (B de la paire précédente devient A de la suivante).
    bool interpolate(D3DContext& d3d, const GpuTexture& a, uint64_t a_id, const GpuTexture& b, uint64_t b_id,
                     float t, const GpuTexture& out);

    // VFG chargé et fonctionnel (le pipeline peut alors se passer du flow maison).
    bool vfg_ready() const;

    // Oublie les textures enregistrées et les entrées en cache (historique vidé, tailles changées).
    void release_textures();

    const char* vsr_status() const;
    const char* vfg_status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
