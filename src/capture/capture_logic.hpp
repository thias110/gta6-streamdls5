#pragma once
// Logique pure de la source « carte de capture » (Media Foundation) : horodatage, choix du mode natif, choix
// du format de sortie demandé au Source Reader, choix du périphérique. Aucun en-tête Windows : testée seule
// (tests/capture_logic_test.cpp), y compris hors Windows.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace capture_logic {

// --- Horodatage ----------------------------------------------------------------------------------------

constexpr double kHnsPerSecond = 1e7;  // Media Foundation compte en unités de 100 ns
inline double hns_to_seconds(int64_t hns) { return double(hns) / kHnsPerSecond; }

// Convertit l'horodatage d'un échantillon MF en secondes sur l'horloge now_seconds() (QPC / fréquence QPC).
//  1. MFSampleExtension_DeviceReferenceSystemTime (QPC du pilote à la capture, en 100 ns) s'il est présent et
//     plausible (pas dans le futur, retard < kMaxLag) : c'est directement l'horloge de now_seconds().
//  2. Sinon l'horodatage MF de l'échantillon (100 ns, origine inconnue) recalé sur l'arrivée : décalage =
//     plus petit (arrivée - horodatage) observé, qui suit lentement une dérive d'horloge (kDriftPerSecond) et
//     repart de zéro après une discontinuité, un retour en arrière ou un trou > kResyncGap. La gigue de
//     livraison (USB, ordonnanceur) disparaît ; le résultat n'est jamais postérieur à l'arrivée.
//  3. Sans horodatage (absent, négatif, ou figé : même valeur deux fois de suite, ex. toujours 0) : l'arrivée.
// Garde-fous : le résultat ne recule jamais et ne se répète pas (+kMinStep au moins) hors discontinuité, et
// n'est jamais postérieur à l'arrivée.
class TimestampMapper {
public:
    enum class Basis { None, Device, Mapped, Arrival };

    static constexpr double kMaxLag         = 0.25;    // s : au-delà, l'horodatage n'est pas crédible
    static constexpr double kFutureSlack    = 0.002;   // s : tolérance d'un horodatage pilote « en avance »
    static constexpr double kDriftPerSecond = 0.0002;  // remontée max du décalage (0,2 ms par seconde, 200 ppm)
    static constexpr double kResyncGap      = 1.0;     // s : trou d'horodatage qui impose un recalage
    static constexpr double kMinStep        = 1e-4;    // s : écart minimal entre deux images (garde-fou)

    // device_qpc_hns : MFSampleExtension_DeviceReferenceSystemTime ; sample_hns : IMFSample::GetSampleTime ;
    // discontinuity : MFSampleExtension_Discontinuity, tick de flux ou changement de format ;
    // arrival : now_seconds() à la sortie de ReadSample.
    double map(std::optional<int64_t> device_qpc_hns, std::optional<int64_t> sample_hns, bool discontinuity,
               double arrival);
    Basis  basis() const { return basis_; }
    void   reset() { *this = TimestampMapper{}; }

private:
    double map_raw(std::optional<int64_t> device_qpc_hns, std::optional<int64_t> sample_hns, bool discontinuity,
                   double arrival);

    Basis  basis_    = Basis::None;
    bool   have_out_ = false;
    double last_out_ = 0.0;
    bool   anchored_ = false;
    double offset_   = 0.0;  // arrivée - horodatage (s), borne basse observée
    double last_s_   = 0.0;  // dernier horodatage MF (s)
};

const char* basis_name(TimestampMapper::Basis b);

// --- Modes natifs ----------------------------------------------------------------------------------------

enum class Subtype { NV12, YUY2, MJPG, RGB32, P010, H264, HEVC, Other };
const char* subtype_name(Subtype s);
bool        is_uncompressed(Subtype s);

struct NativeMode {
    uint32_t width = 0, height = 0;
    double   fps   = 0.0;
    Subtype  subtype = Subtype::Other;
};
std::string describe(const NativeMode& m);  // "1920x1080 @ 60.00 fps, NV12"

struct ModeRequest {
    uint32_t width = 0, height = 0;  // 0 = automatique
    double   fps   = 0.0;            // 0 = n'importe quelle fréquence
    bool     automatic() const { return width == 0; }
};
// "" (auto), "1920x1080", "1920x1080@60", "1280x720@59.94". false + message si la syntaxe est fausse.
bool parse_mode(std::string_view text, ModeRequest& out, std::string* error = nullptr);

// Index du mode retenu, -1 si aucun. Automatique : >= 59 fps d'abord, puis la plus grande résolution (bornée
// à la 4K), puis la plus haute fréquence (<= 144), puis non compressé (moins de latence que le MJPEG).
// Demandé : taille exacte. Fréquence donnée : à +-1 fps près, non compressé préféré, puis la plus proche.
// Taille seule (--mode WxH) : la plus haute fréquence (>= 59 d'abord, <= 144, 59.94 ~ 60), puis non compressé.
int choose_mode(const std::vector<NativeMode>& modes, const ModeRequest& req);

// --- Format de sortie demandé au Source Reader -------------------------------------------------------------

// NV12 : le Source Reader convertit (YUY2, MJPEG, P010, H.264...) puis le VideoProcessor D3D11 passe en BGRA.
// YUY2 : repli si le NV12 est refusé (le VideoProcessor lit aussi le 4:2:2 entrelacé).
// RGB32 : dernier repli, ou format natif déjà RGB (pas d'aller-retour YUV).
enum class Output { NV12, YUY2, RGB32 };
const char*         output_name(Output o);
std::vector<Output> output_order(Subtype native);
// Format effectivement livré (MF_MT_SUBTYPE du type courant) : uniquement NV12, YUY2 ou RGB32 ; tout autre
// sous-type (ou inconnu) -> nullopt, jamais interprété comme du NV12.
std::optional<Output> output_from_subtype(Subtype s);

// --- Périphérique ---------------------------------------------------------------------------------------

// needle non vide : premier nom qui le contient (casse ignorée), -1 sinon. Vide : premier nom qui contient
// un indice de carte de capture connu (Elgato, AVerMedia, « capture », « HDMI »...), sinon le premier.
int pick_device(const std::vector<std::wstring>& names, std::wstring_view needle);

}  // namespace capture_logic
