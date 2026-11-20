#include "nvvfx_backend.hpp"

#if !defined(RPENHANCE_HAVE_NVVFX)

// ---------------------------------------------------------------------------------------------------
// Compilé sans RPENHANCE_NVVFX : aucune dépendance NVIDIA, tout renvoie false (repli du pipeline).
// ---------------------------------------------------------------------------------------------------
namespace {
constexpr const char* kCompiledOut = "non compilé (option CMake RPENHANCE_NVVFX=OFF) : pipeline actuel utilisé";
}

struct NvvfxBackend::Impl {};

NvvfxBackend::NvvfxBackend()  = default;
NvvfxBackend::~NvvfxBackend() = default;
bool NvvfxBackend::compiled_in() { return false; }
void NvvfxBackend::set_options(const NvvfxOptions&) {}
bool NvvfxBackend::upscale(D3DContext&, const GpuTexture&, const GpuTexture&) { return false; }
bool NvvfxBackend::interpolate(D3DContext&, const GpuTexture&, uint64_t, const GpuTexture&, uint64_t, float,
                               const GpuTexture&) {
    return false;
}
bool        NvvfxBackend::vfg_ready() const { return false; }
void        NvvfxBackend::release_textures() {}
const char* NvvfxBackend::vsr_status() const { return kCompiledOut; }
const char* NvvfxBackend::vfg_status() const { return kCompiledOut; }

#else  // RPENHANCE_HAVE_NVVFX

// ---------------------------------------------------------------------------------------------------
// En-têtes du VFX SDK Core installé localement (NVVFX_SDK_DIR, voir CMakeLists.txt). Ils ne sont pas
// dans le dépôt (licence NVIDIA) et ne servent qu'aux types (NvCVImage, NvCV_Status...) et aux chaînes
// de sélecteurs : aucune fonction du SDK n'est liée statiquement, tout passe par GetProcAddress.
//
// On n'utilise jamais les constructeurs/destructeur C++ inline de NvCVImage (ils appellent
// NvCVImage_Alloc/Dealloc et imposeraient de lier NVCVImage.lib) : les images sont créées et détruites
// par l'API C (NvCVImage_Create / NvCVImage_Destroy) et manipulées par pointeur.
// ---------------------------------------------------------------------------------------------------
#include "nvCVImage.h"
#include "nvVideoEffects.h"
#if __has_include("nvVFXVideoFrameGeneration.h")
#include "nvVFXVideoFrameGeneration.h"
#endif
#if __has_include("nvVFXVideoSuperRes.h")
#include "nvVFXVideoSuperRes.h"
#endif

#include <cmath>

// Sélecteurs : uniquement ceux définis par les en-têtes du SDK installé, aucune chaîne devinée ici.
// La doc 1.3 et les exemples officiels ne nomment pas toujours pareil l'effet et le mode de VSR :
// on accepte les deux orthographes.
#if defined(NVVFX_FX_VIDEO_SUPER_RES)
#define RPE_FX_VSR NVVFX_FX_VIDEO_SUPER_RES  // exemples VFX-SDK-Samples 1.3
#elif defined(NVVFX_FX_SUPER_RES)
#define RPE_FX_VSR NVVFX_FX_SUPER_RES  // tableau des sélecteurs du guide 1.3
#else
#error "VFX SDK : sélecteur d'effet VSR introuvable (NVVFX_FX_VIDEO_SUPER_RES / NVVFX_FX_SUPER_RES). SDK trop ancien ?"
#endif
#if defined(NVVFX_QUALITY_LEVEL)
#define RPE_VSR_MODE NVVFX_QUALITY_LEVEL  // exemples VFX-SDK-Samples 1.3
#elif defined(NVVFX_MODE)
#define RPE_VSR_MODE NVVFX_MODE  // guide 1.3
#else
#error "VFX SDK : sélecteur de mode VSR introuvable (NVVFX_QUALITY_LEVEL / NVVFX_MODE)."
#endif
#if !defined(NVVFX_FX_VIDEO_FRAME_GENERATION) || !defined(NVVFXVIDEOFRAMEGENERATION_MODE) ||        \
    !defined(NVVFXVIDEOFRAMEGENERATION_FRAME_MULTIPLIER) || !defined(NVVFXVIDEOFRAMEGENERATION_TIMESTEP) || \
    !defined(NVVFXVIDEOFRAMEGENERATION_SHOT_CHANGE)
#error "VFX SDK : sélecteurs Video Frame Generation introuvables. Il faut le SDK 1.3+ et la feature nvVFXVideoFrameGeneration installée (scripts/install-nvvfx.ps1) ; vérifier NVVFX_SDK_DIR."
#endif
#if !defined(NVVFX_INPUT_IMAGE_0) || !defined(NVVFX_INPUT_IMAGE_1) || !defined(NVVFX_OUTPUT_IMAGE) || \
    !defined(NVVFX_INPUT_WIDTH) || !defined(NVVFX_INPUT_HEIGHT) || !defined(NVVFX_CUDA_STREAM) ||    \
    !defined(NVVFX_MODEL_DIRECTORY) || !defined(NVVFX_STRENGTH)
#error "VFX SDK : sélecteurs de base introuvables dans nvVideoEffects.h (SDK trop ancien ?)."
#endif

namespace {

// Pointeurs de fonctions (ABI C x64 : une seule convention d'appel). Signatures du guide VFX SDK 1.3 et
// du guide NvCVImage.
struct Api {
    HMODULE vfx = nullptr, img = nullptr;

    NvCV_Status (*CreateEffect)(NvVFX_EffectSelector, NvVFX_Handle*)                  = nullptr;
    void (*DestroyEffect)(NvVFX_Handle)                                                = nullptr;
    NvCV_Status (*SetU32)(NvVFX_Handle, NvVFX_ParameterSelector, unsigned int)         = nullptr;
    NvCV_Status (*SetF32)(NvVFX_Handle, NvVFX_ParameterSelector, float)                = nullptr;
    NvCV_Status (*SetString)(NvVFX_Handle, NvVFX_ParameterSelector, const char*)       = nullptr;
    NvCV_Status (*SetImage)(NvVFX_Handle, NvVFX_ParameterSelector, NvCVImage*)         = nullptr;
    NvCV_Status (*SetCudaStream)(NvVFX_Handle, NvVFX_ParameterSelector, CUstream)      = nullptr;
    NvCV_Status (*Load)(NvVFX_Handle)                                                  = nullptr;
    NvCV_Status (*Run)(NvVFX_Handle, int)                                              = nullptr;
    NvCV_Status (*CudaStreamCreate)(CUstream*)                                         = nullptr;
    void (*CudaStreamDestroy)(CUstream)                                                = nullptr;
    NvCV_Status (*GetVersion)(unsigned int*)                                           = nullptr;  // optionnel

    NvCV_Status (*ImgCreate)(unsigned, unsigned, NvCVImage_PixelFormat, NvCVImage_ComponentType, unsigned, unsigned,
                             unsigned, NvCVImage**)                                    = nullptr;
    void (*ImgDestroy)(NvCVImage*)                                                     = nullptr;
    NvCV_Status (*ImgTransfer)(const NvCVImage*, NvCVImage*, float, struct CUstream_st*, NvCVImage*) = nullptr;
    NvCV_Status (*ImgMap)(NvCVImage*, struct CUstream_st*)                             = nullptr;
    NvCV_Status (*ImgUnmap)(NvCVImage*, struct CUstream_st*)                           = nullptr;
    NvCV_Status (*ImgInitFromD3D)(NvCVImage*, ID3D11Texture2D*)                        = nullptr;
    const char* (*ErrorString)(NvCV_Status)                                            = nullptr;  // optionnel
};

template <class F>
bool resolve(HMODULE primary, HMODULE secondary, const char* name, F& out) {
    out = nullptr;
    for (HMODULE m : {primary, secondary}) {
        if (!m) continue;
        if (FARPROC p = GetProcAddress(m, name)) {
            out = reinterpret_cast<F>(reinterpret_cast<void (*)()>(p));  // via void(*)() : cast générique
            return true;
        }
    }
    return false;
}

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool dir_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::wstring with_slash(std::wstring p) {
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    if (!p.empty() && p.back() != L'\\') p += L'\\';
    return p;
}
std::wstring env(const wchar_t* name) {
    wchar_t buf[4096];
    const DWORD n = GetEnvironmentVariableW(name, buf, DWORD(std::size(buf)));
    return (n > 0 && n < std::size(buf)) ? std::wstring(buf, n) : std::wstring();
}

// Le SDK charge lui-même les bibliothèques des features (<sdk>\features\<feature>\bin) ; leurs
// dépendances (TensorRT, CUDA...) sont trouvées via le PATH, comme dans les scripts run_*.bat des
// exemples officiels. On préfixe donc le PATH du processus (et de lui seul).
void prepend_path(const std::vector<std::wstring>& dirs) {
    std::wstring path = env(L"PATH"), add;
    for (const auto& d : dirs) {
        if (d.empty() || path.find(d) != std::wstring::npos) continue;
        add += d;
        add += L';';
    }
    if (!add.empty()) SetEnvironmentVariableW(L"PATH", (add + path).c_str());
}
std::vector<std::wstring> feature_dirs(const std::wstring& root) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"features\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        for (const wchar_t* sub : {L"\\bin", L"\\lib"}) {
            const std::wstring d = root + L"features\\" + fd.cFileName + sub;
            if (dir_exists(d)) out.push_back(d);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// Texture D3D11 enregistrée auprès de CUDA (cudaGraphicsResource, via NvCVImage_InitFromD3DTexture).
struct RegTex {
    ComPtr<ID3D11Texture2D> tex;  // garde la texture vivante : son adresse ne peut pas être réutilisée
    NvCVImage*              img      = nullptr;
    uint64_t                last_use = 0;
};

bool is_bgra8(const GpuTexture& t) { return t.tex && t.format == DXGI_FORMAT_B8G8R8A8_UNORM; }

}  // namespace

struct NvvfxBackend::Impl {
    NvvfxOptions opts;
    Api          api;
    bool         tried  = false;  // chargement des DLL tenté
    bool         loaded = false;
    std::wstring root;            // racine du SDK retenue
    CUstream     stream = nullptr;
    NvCVImage*   tmp    = nullptr;  // tampon intermédiaire réutilisé par NvCVImage_Transfer
    std::string  load_status = "pas encore utilisé";

    // VSR
    NvVFX_Handle vsr = nullptr;
    Size2D       vsr_in{}, vsr_out{};
    bool         vsr_failed = false;  // pour cette paire de tailles ; réessayé si elles changent
    NvCVImage *  vsr_src = nullptr, *vsr_dst = nullptr;
    std::string  vsr_status = "pas encore utilisé";

    // VFG
    NvVFX_Handle vfg = nullptr;
    Size2D       vfg_size{};
    bool         vfg_failed = false;  // pour cette taille ; réessayé si elle change
    bool         vfg_loaded = false;
    NvCVImage*   vfg_in[2]{};
    NvCVImage*   vfg_out = nullptr;
    uint64_t     in_id[2]{};  // contenu actuel de vfg_in[0/1] (0 = rien)
    uint64_t     last_a = 0, last_b = 0;
    float        last_t   = -1.0f;
    ID3D11Texture2D* last_target = nullptr;
    std::string  vfg_status = "pas encore utilisé";

    std::vector<RegTex> regs;
    uint64_t            clock = 0;
    static constexpr size_t kMaxRegs = 16;

    ~Impl() { shutdown(); }

    std::string err(NvCV_Status s) const {
        char buf[160];
        const char* txt = api.ErrorString ? api.ErrorString(s) : nullptr;
        std::snprintf(buf, sizeof(buf), "code %d%s%s", int(s), txt ? " : " : "", txt ? txt : "");
        return buf;
    }

    void destroy_image(NvCVImage*& im) {
        if (im && api.ImgDestroy) api.ImgDestroy(im);
        im = nullptr;
    }
    bool create_image(NvCVImage*& im, unsigned w, unsigned h) {
        destroy_image(im);
        return api.ImgCreate(w, h, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32, &im) == NVCV_SUCCESS && im;
    }

    void destroy_vsr() {
        if (vsr && api.DestroyEffect) api.DestroyEffect(vsr);
        vsr = nullptr;
        destroy_image(vsr_src);
        destroy_image(vsr_dst);
        vsr_in = vsr_out = Size2D{};
    }
    void destroy_vfg() {
        if (vfg && api.DestroyEffect) api.DestroyEffect(vfg);
        vfg        = nullptr;
        vfg_loaded = false;
        destroy_image(vfg_in[0]);
        destroy_image(vfg_in[1]);
        destroy_image(vfg_out);
        in_id[0] = in_id[1] = 0;
        last_a = last_b = 0;
        last_target     = nullptr;
        vfg_size        = Size2D{};
    }
    void release_regs() {
        // NvCVImage_Destroy -> Dealloc : désenregistre la ressource CUDA de la texture.
        for (auto& r : regs) destroy_image(r.img);
        regs.clear();
        in_id[0] = in_id[1] = 0;
        last_a = last_b = 0;
        last_target     = nullptr;
    }
    void shutdown() {
        destroy_vsr();
        destroy_vfg();
        release_regs();
        destroy_image(tmp);
        if (stream && api.CudaStreamDestroy) api.CudaStreamDestroy(stream);
        stream = nullptr;
        // Les DLL restent chargées jusqu'à la fin du processus (le SDK peut garder des threads/contextes).
    }

    // ------------------------------------------------------------------------------------------------
    // Chargement dynamique du SDK (une seule tentative ; l'échec est définitif pour la session).
    // ------------------------------------------------------------------------------------------------
    bool ensure_loaded(D3DContext& d3d) {
        if (tried) return loaded;
        tried = true;
        auto fail = [&](std::string why) {
            load_status = std::move(why);
            log_msg("NVIDIA VFX SDK indisponible : %s -> repli sur le pipeline actuel (flow maison / VSR VideoProcessor)",
                    load_status.c_str());
            return false;
        };
        if (!d3d.is_nvidia()) return fail("GPU non NVIDIA (" + d3d.adapter_name() + ")");

        std::vector<std::wstring> roots;
        auto add = [&](const std::wstring& r) {
            if (!r.empty()) roots.push_back(with_slash(r));
        };
        add(opts.sdk_dir);
        add(env(L"NVVFX_SDK_DIR"));
        add(env(L"VFXSDK_ROOT"));
        add(exe_directory() + L"nvvfx-sdk");
#ifdef RPENHANCE_NVVFX_DEFAULT_DIR
        add(widen(RPENHANCE_NVVFX_DEFAULT_DIR));
#endif
        std::wstring dll_dir;
        for (const auto& r : roots) {
            for (const wchar_t* sub : {L"bin\\", L"lib\\", L""}) {
                if (file_exists(r + sub + L"NVVideoEffects.dll")) {
                    root    = r;
                    dll_dir = r + sub;
                    break;
                }
            }
            if (!dll_dir.empty()) break;
        }
        if (dll_dir.empty())
            return fail("NVVideoEffects.dll introuvable (--nvvfx-dir, NVVFX_SDK_DIR, VFXSDK_ROOT, <exe>\\nvvfx-sdk ; "
                        "voir scripts\\install-nvvfx.ps1)");

        std::vector<std::wstring> dirs{dll_dir.substr(0, dll_dir.size() - 1)};
        for (auto& d : feature_dirs(root)) dirs.push_back(d);
        prepend_path(dirs);

        // LOAD_WITH_ALTERED_SEARCH_PATH : les dépendances sont d'abord cherchées à côté de la DLL.
        api.img = LoadLibraryExW((dll_dir + L"NVCVImage.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        api.vfx = LoadLibraryExW((dll_dir + L"NVVideoEffects.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!api.vfx) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "LoadLibrary(NVVideoEffects.dll) a échoué (erreur Windows %lu)",
                          static_cast<unsigned long>(GetLastError()));
            return fail(buf);
        }

        bool ok = true;
        ok &= resolve(api.vfx, nullptr, "NvVFX_CreateEffect", api.CreateEffect);
        ok &= resolve(api.vfx, nullptr, "NvVFX_DestroyEffect", api.DestroyEffect);
        ok &= resolve(api.vfx, nullptr, "NvVFX_SetU32", api.SetU32);
        ok &= resolve(api.vfx, nullptr, "NvVFX_SetF32", api.SetF32);
        ok &= resolve(api.vfx, nullptr, "NvVFX_SetString", api.SetString);
        ok &= resolve(api.vfx, nullptr, "NvVFX_SetImage", api.SetImage);
        ok &= resolve(api.vfx, nullptr, "NvVFX_SetCudaStream", api.SetCudaStream);
        ok &= resolve(api.vfx, nullptr, "NvVFX_Load", api.Load);
        ok &= resolve(api.vfx, nullptr, "NvVFX_Run", api.Run);
        ok &= resolve(api.vfx, nullptr, "NvVFX_CudaStreamCreate", api.CudaStreamCreate);
        ok &= resolve(api.vfx, nullptr, "NvVFX_CudaStreamDestroy", api.CudaStreamDestroy);
        resolve(api.vfx, nullptr, "NvVFX_GetVersion", api.GetVersion);
        // NvCVImage_* : NVCVImage.dll, sinon NVVideoEffects.dll.
        ok &= resolve(api.img, api.vfx, "NvCVImage_Create", api.ImgCreate);
        ok &= resolve(api.img, api.vfx, "NvCVImage_Destroy", api.ImgDestroy);
        ok &= resolve(api.img, api.vfx, "NvCVImage_Transfer", api.ImgTransfer);
        ok &= resolve(api.img, api.vfx, "NvCVImage_MapResource", api.ImgMap);
        ok &= resolve(api.img, api.vfx, "NvCVImage_UnmapResource", api.ImgUnmap);
        // Nom documenté (guide NvCVImage 1.0.1), puis nom de l'en-tête MIT historique (0.7.x).
        if (!resolve(api.img, api.vfx, "NvCVImage_InitFromD3DTexture", api.ImgInitFromD3D))
            ok &= resolve(api.img, api.vfx, "NvCVImage_InitFromD3D11Texture", api.ImgInitFromD3D);
        resolve(api.img, api.vfx, "NvCV_GetErrorStringFromCode", api.ErrorString);
        if (!ok) return fail("DLL du SDK incomplète (fonction manquante : version du SDK incompatible ?)");

        NvCV_Status s = api.CudaStreamCreate(&stream);
        if (s != NVCV_SUCCESS) return fail("NvVFX_CudaStreamCreate : " + err(s));
        s = api.ImgCreate(0, 0, NVCV_FORMAT_UNKNOWN, NVCV_TYPE_UNKNOWN, NVCV_CHUNKY, NVCV_CPU, 0, &tmp);
        if (s != NVCV_SUCCESS) return fail("NvCVImage_Create (tampon de transfert) : " + err(s));

        unsigned ver = 0;
        if (api.GetVersion) api.GetVersion(&ver);
        loaded      = true;
        load_status = "chargé";
        log_msg("NVIDIA VFX SDK chargé depuis %s (version 0x%08X)", narrow(dll_dir).c_str(), ver);
        return true;
    }

    // ------------------------------------------------------------------------------------------------
    // Interop D3D11 <-> CUDA.
    //
    // Synchronisation (un seul thread : celui qui pilote le contexte immédiat D3D11) :
    //  - NvCVImage_MapResource enveloppe cudaGraphicsMapResources sur `stream` : le travail D3D11 émis
    //    AVANT le map est terminé avant que le travail CUDA émis ensuite sur ce flux ne commence
    //    (garantie de l'API CUDA). On fait quand même un Flush() du contexte immédiat avant de mapper,
    //    pour que les commandes en attente côté pilote soient soumises.
    //  - NvCVImage_UnmapResource enveloppe cudaGraphicsUnmapResources : le travail CUDA émis AVANT
    //    l'unmap sur ce flux est terminé avant tout travail D3D11 émis APRÈS. Le compositeur peut donc
    //    lire la texture de sortie juste après, sans attente CPU (pas de cudaStreamSynchronize).
    //  - Les filtres tournent en asynchrone (NvVFX_Run(h, 1)) sur le même flux : ordre garanti.
    // Une texture ne doit être ni mappée ni lue par D3D en même temps : on mappe, copie, démappe
    // immédiatement.
    // ------------------------------------------------------------------------------------------------
    NvCVImage* registered(ID3D11Texture2D* t) {
        for (auto& r : regs) {
            if (r.tex.Get() == t) {
                r.last_use = ++clock;
                return r.img;
            }
        }
        if (regs.size() >= kMaxRegs) {
            auto lru = std::min_element(regs.begin(), regs.end(),
                                        [](const RegTex& a, const RegTex& b) { return a.last_use < b.last_use; });
            destroy_image(lru->img);
            regs.erase(lru);
            in_id[0] = in_id[1] = 0;  // prudence : les tampons d'entrée pouvaient venir de cette texture
        }
        NvCVImage*  im = nullptr;
        NvCV_Status s  = api.ImgCreate(0, 0, NVCV_FORMAT_UNKNOWN, NVCV_TYPE_UNKNOWN, NVCV_CHUNKY, NVCV_CPU, 0, &im);
        if (s == NVCV_SUCCESS && im) s = api.ImgInitFromD3D(im, t);
        if (s != NVCV_SUCCESS || !im) {
            log_msg("NVIDIA VFX : NvCVImage_InitFromD3DTexture a échoué (%s)", err(s).c_str());
            destroy_image(im);
            return nullptr;
        }
        RegTex r;
        r.tex      = t;
        r.img      = im;
        r.last_use = ++clock;
        regs.push_back(r);
        return im;
    }

    // Texture -> tampon CUDA linéaire (ou l'inverse si to_texture).
    bool copy(ID3D11Texture2D* t, NvCVImage* buf, bool to_texture, std::string& why) {
        NvCVImage* r = registered(t);
        if (!r) {
            why = "enregistrement CUDA de la texture D3D11 impossible";
            return false;
        }
        NvCV_Status s = api.ImgMap(r, stream);
        if (s != NVCV_SUCCESS) {
            why = "NvCVImage_MapResource : " + err(s);
            return false;
        }
        s = to_texture ? api.ImgTransfer(buf, r, 1.0f, stream, tmp) : api.ImgTransfer(r, buf, 1.0f, stream, tmp);
        const NvCV_Status u = api.ImgUnmap(r, stream);
        if (s != NVCV_SUCCESS) {
            why = "NvCVImage_Transfer : " + err(s);
            return false;
        }
        if (u != NVCV_SUCCESS) {
            why = "NvCVImage_UnmapResource : " + err(u);
            return false;
        }
        return true;
    }

    bool set_model_dir(NvVFX_Handle h) {
        if (opts.model_dir.empty()) return true;  // le SDK 1.3 trouve les modèles des features installées
        return api.SetString(h, NVVFX_MODEL_DIRECTORY, narrow(opts.model_dir).c_str()) == NVCV_SUCCESS;
    }

    // ------------------------------------------------------------------------------------------------
    // VSR
    // ------------------------------------------------------------------------------------------------
    bool ensure_vsr(Size2D in, Size2D out) {
        if (vsr && vsr_in == in && vsr_out == out) return true;
        if (vsr_failed && vsr_in == in && vsr_out == out) return false;
        destroy_vsr();
        vsr_in     = in;
        vsr_out    = out;
        vsr_failed = true;  // remis à false en fin de configuration réussie
        auto fail  = [&](const char* step, NvCV_Status s) {
            vsr_status = std::string(step) + " : " + err(s);
            log_msg("NVIDIA VFX VSR %ux%u -> %ux%u : %s -> repli sur le VSR du VideoProcessor", in.width, in.height,
                    out.width, out.height, vsr_status.c_str());
            if (vsr && api.DestroyEffect) api.DestroyEffect(vsr);
            vsr = nullptr;
            return false;
        };
        NvCV_Status s = api.CreateEffect(RPE_FX_VSR, &vsr);
        if (s != NVCV_SUCCESS) return fail("NvVFX_CreateEffect (feature nvvfxvideosuperres installée ?)", s);
        if (!set_model_dir(vsr)) return fail("NVVFX_MODEL_DIRECTORY", NVCV_ERR_PARAMETER);
        if (!create_image(vsr_src, in.width, in.height) || !create_image(vsr_dst, out.width, out.height))
            return fail("NvCVImage_Create (GPU)", NVCV_ERR_MEMORY);
        if ((s = api.SetImage(vsr, NVVFX_INPUT_IMAGE, vsr_src)) != NVCV_SUCCESS) return fail("entrée", s);
        if ((s = api.SetImage(vsr, NVVFX_OUTPUT_IMAGE, vsr_dst)) != NVCV_SUCCESS) return fail("sortie", s);
        if ((s = api.SetCudaStream(vsr, NVVFX_CUDA_STREAM, stream)) != NVCV_SUCCESS) return fail("flux CUDA", s);
#if defined(NVVFX_IMAGE_ENCODING_MODE) && defined(NVVFX_IMAGE_ENCODING_RGB8)
        if ((s = api.SetU32(vsr, NVVFX_IMAGE_ENCODING_MODE, NVVFX_IMAGE_ENCODING_RGB8)) != NVCV_SUCCESS)
            return fail("encodage 8 bits", s);
#endif
        if ((s = api.SetU32(vsr, RPE_VSR_MODE, opts.vsr_mode)) != NVCV_SUCCESS) return fail("mode", s);
        if (opts.vsr_strength >= 0.0f &&
            (s = api.SetF32(vsr, NVVFX_STRENGTH, std::clamp(opts.vsr_strength, 0.0f, 1.0f))) != NVCV_SUCCESS)
            return fail("strength", s);
        log_msg("NVIDIA VFX VSR : chargement %ux%u -> %ux%u, mode %u (peut prendre du temps la première fois)",
                in.width, in.height, out.width, out.height, opts.vsr_mode);
        if ((s = api.Load(vsr)) != NVCV_SUCCESS) return fail("NvVFX_Load", s);
        vsr_failed = false;
        vsr_status = "actif (SDK NVIDIA Video Effects)";
        log_msg("NVIDIA VFX VSR actif");
        return true;
    }

    bool upscale(D3DContext& d3d, const GpuTexture& src, const GpuTexture& dst) {
        if (!ensure_loaded(d3d)) {
            vsr_status = load_status;
            return false;
        }
        if (!is_bgra8(src) || !is_bgra8(dst)) {
            vsr_status = "format non BGRA8";
            return false;
        }
        const Size2D in = src.size(), out = dst.size();
        if (out.width <= in.width || out.height <= in.height) {
            vsr_status = "sortie pas plus grande que l'entrée : VSR NVIDIA non utilisé";
            return false;
        }
        if (!ensure_vsr(in, out)) return false;
        std::string why;
        d3d.ctx()->Flush();
        NvCV_Status s = NVCV_SUCCESS;
        if (!copy(src.tex.Get(), vsr_src, false, why) || (s = api.Run(vsr, 1)) != NVCV_SUCCESS ||
            !copy(dst.tex.Get(), vsr_dst, true, why)) {
            if (why.empty()) why = "NvVFX_Run : " + err(s);
            vsr_status = why;
            vsr_failed = true;  // pas de nouvel essai pour ces tailles
            if (vsr && api.DestroyEffect) api.DestroyEffect(vsr);
            vsr = nullptr;
            log_msg("NVIDIA VFX VSR en échec : %s -> repli sur le VSR du VideoProcessor", why.c_str());
            return false;
        }
        return true;
    }

    // ------------------------------------------------------------------------------------------------
    // VFG
    // ------------------------------------------------------------------------------------------------
    bool ensure_vfg(Size2D size) {
        if (vfg_loaded && vfg_size == size) return true;
        if (vfg_failed && vfg_size == size) return false;
        destroy_vfg();
        vfg_size   = size;
        vfg_failed = true;
        auto fail  = [&](const char* step, NvCV_Status s) {
            vfg_status = std::string(step) + " : " + err(s);
            log_msg("NVIDIA VFX VFG %ux%u : %s -> repli sur l'interpolation maison (flow)", size.width, size.height,
                    vfg_status.c_str());
            if (vfg && api.DestroyEffect) api.DestroyEffect(vfg);
            vfg = nullptr;
            return false;
        };
        // VFG : GPU Ada ou Blackwell et pilote >= 570.65 sous Windows (guide 1.3). Sur un autre GPU,
        // c'est la création ou le chargement qui échoue : repli.
        NvCV_Status s = api.CreateEffect(NVVFX_FX_VIDEO_FRAME_GENERATION, &vfg);
        if (s != NVCV_SUCCESS)
            return fail("NvVFX_CreateEffect (feature nvVFXVideoFrameGeneration installée ? GPU Ada/Blackwell ?)", s);
        if (!set_model_dir(vfg)) return fail("NVVFX_MODEL_DIRECTORY", NVCV_ERR_PARAMETER);
        if (!create_image(vfg_in[0], size.width, size.height) || !create_image(vfg_in[1], size.width, size.height) ||
            !create_image(vfg_out, size.width, size.height))
            return fail("NvCVImage_Create (GPU)", NVCV_ERR_MEMORY);
        // Ordre de l'exemple officiel VideoFrameGenerationEffectApp : flux, taille, mode, puis Load.
        if ((s = api.SetCudaStream(vfg, NVVFX_CUDA_STREAM, stream)) != NVCV_SUCCESS) return fail("flux CUDA", s);
        if ((s = api.SetU32(vfg, NVVFX_INPUT_WIDTH, size.width)) != NVCV_SUCCESS) return fail("largeur", s);
        if ((s = api.SetU32(vfg, NVVFX_INPUT_HEIGHT, size.height)) != NVCV_SUCCESS) return fail("hauteur", s);
        if ((s = api.SetU32(vfg, NVVFXVIDEOFRAMEGENERATION_MODE, std::min(opts.vfg_mode, 2u))) != NVCV_SUCCESS)
            return fail("mode", s);
#if defined(NVVFXVIDEOFRAMEGENERATION_AUTOMATIC_SHOT_CHANGE_DETECTION_ENABLED)
        // Détection automatique gardée (défaut) : rpenhance détecte déjà les coupures et n'appelle pas VFG
        // à travers (HOLD), celle du SDK rattrape celles qu'il manquerait (copie de B au lieu d'un fondu).
        if ((s = api.SetU32(vfg, NVVFXVIDEOFRAMEGENERATION_AUTOMATIC_SHOT_CHANGE_DETECTION_ENABLED, 1u)) !=
            NVCV_SUCCESS)
            return fail("détection de coupure", s);
#endif
        log_msg("NVIDIA VFX VFG : chargement %ux%u, mode %u (peut prendre du temps la première fois)", size.width,
                size.height, std::min(opts.vfg_mode, 2u));
        if ((s = api.Load(vfg)) != NVCV_SUCCESS) return fail("NvVFX_Load", s);
        if ((s = api.SetImage(vfg, NVVFX_OUTPUT_IMAGE, vfg_out)) != NVCV_SUCCESS) return fail("sortie", s);
        // Mode « timestep explicite » : multiplicateur à 0, puis Timestep avant chaque Run.
        if ((s = api.SetU32(vfg, NVVFXVIDEOFRAMEGENERATION_FRAME_MULTIPLIER, 0u)) != NVCV_SUCCESS)
            return fail("mode timestep", s);
        if ((s = api.SetU32(vfg, NVVFXVIDEOFRAMEGENERATION_SHOT_CHANGE, 0u)) != NVCV_SUCCESS)
            return fail("shot change", s);
        vfg_failed = false;
        vfg_loaded = true;
        vfg_status = "actif (SDK NVIDIA Video Effects)";
        log_msg("NVIDIA VFX VFG actif");
        return true;
    }

    bool interpolate(D3DContext& d3d, const GpuTexture& a, uint64_t a_id, const GpuTexture& b, uint64_t b_id, float t,
                     const GpuTexture& out) {
        if (!ensure_loaded(d3d)) {
            vfg_status = load_status;
            return false;
        }
        if (!is_bgra8(a) || !is_bgra8(b) || !is_bgra8(out) || !(a.size() == b.size()) || !(a.size() == out.size())) {
            vfg_status = "entrées/sortie : tailles ou formats différents";
            return false;
        }
        if (!ensure_vfg(a.size())) return false;
        t = std::clamp(t, 0.001f, 0.999f);  // intervalle ouvert (0, 1) exigé par le SDK

        // Même paire, même instant, même cible : l'image est déjà dans `out`.
        if (a_id == last_a && b_id == last_b && std::abs(t - last_t) < 1e-4f && out.tex.Get() == last_target)
            return true;

        std::string why;
        auto fail = [&](std::string w) {
            vfg_status = std::move(w);
            vfg_failed = true;
            vfg_loaded = false;
            in_id[0] = in_id[1] = 0;
            if (vfg && api.DestroyEffect) api.DestroyEffect(vfg);
            vfg = nullptr;
            log_msg("NVIDIA VFX VFG en échec : %s -> repli sur l'interpolation maison (flow)", vfg_status.c_str());
            return false;
        };
        d3d.ctx()->Flush();
        // Entrées : B de la paire précédente devient A de la suivante, sans recopie.
        if (in_id[0] != a_id) {
            if (in_id[1] == a_id) {
                std::swap(vfg_in[0], vfg_in[1]);
                std::swap(in_id[0], in_id[1]);
            } else {
                in_id[0] = 0;
                if (!copy(a.tex.Get(), vfg_in[0], false, why)) return fail(why);
                in_id[0] = a_id;
            }
        }
        if (in_id[1] != b_id) {
            in_id[1] = 0;
            if (!copy(b.tex.Get(), vfg_in[1], false, why)) return fail(why);
            in_id[1] = b_id;
        }
        NvCV_Status s;
        if ((s = api.SetImage(vfg, NVVFX_INPUT_IMAGE_0, vfg_in[0])) != NVCV_SUCCESS) return fail("entrée 0 : " + err(s));
        if ((s = api.SetImage(vfg, NVVFX_INPUT_IMAGE_1, vfg_in[1])) != NVCV_SUCCESS) return fail("entrée 1 : " + err(s));
        if ((s = api.SetF32(vfg, NVVFXVIDEOFRAMEGENERATION_TIMESTEP, t)) != NVCV_SUCCESS)
            return fail("timestep : " + err(s));
        if ((s = api.Run(vfg, 1)) != NVCV_SUCCESS) return fail("NvVFX_Run : " + err(s));
        if (!copy(out.tex.Get(), vfg_out, true, why)) return fail(why);
        last_a      = a_id;
        last_b      = b_id;
        last_t      = t;
        last_target = out.tex.Get();
        return true;
    }
};

NvvfxBackend::NvvfxBackend() : impl_(std::make_unique<Impl>()) {}
NvvfxBackend::~NvvfxBackend() = default;

bool NvvfxBackend::compiled_in() { return true; }

void NvvfxBackend::set_options(const NvvfxOptions& o) {
    if (o == impl_->opts) return;
    if (impl_->tried && o.sdk_dir != impl_->opts.sdk_dir)
        log_msg("NVIDIA VFX : le dossier du SDK ne peut plus changer après le premier chargement (ignoré)");
    impl_->opts = o;
    // Modes / modèles : les filtres seront recréés et rechargés au prochain usage.
    impl_->destroy_vsr();
    impl_->destroy_vfg();
    impl_->vsr_failed = impl_->vfg_failed = false;
}

bool NvvfxBackend::upscale(D3DContext& d3d, const GpuTexture& src, const GpuTexture& dst) {
    return impl_->upscale(d3d, src, dst);
}

bool NvvfxBackend::interpolate(D3DContext& d3d, const GpuTexture& a, uint64_t a_id, const GpuTexture& b,
                               uint64_t b_id, float t, const GpuTexture& out) {
    return impl_->interpolate(d3d, a, a_id, b, b_id, t, out);
}

bool NvvfxBackend::vfg_ready() const { return impl_->loaded && impl_->vfg_loaded && !impl_->vfg_failed; }

void NvvfxBackend::release_textures() { impl_->release_regs(); }

const char* NvvfxBackend::vsr_status() const { return impl_->vsr_status.c_str(); }
const char* NvvfxBackend::vfg_status() const { return impl_->vfg_status.c_str(); }

#endif  // RPENHANCE_HAVE_NVVFX
