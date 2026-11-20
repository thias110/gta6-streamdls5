# rpenhance

[![windows-build](https://github.com/thias110/gta6-streamdls5/actions/workflows/windows-build.yml/badge.svg)](https://github.com/thias110/gta6-streamdls5/actions/workflows/windows-build.yml)

Upscale + interpolation d'images en temps réel sur le flux **PS Remote Play / chiaki** affiché sur PC,
ou sur l'image d'une **carte de capture HDMI** (PS5 branchée par câble, voir « Deux modes »).
Même idée que Veyra, mais sans installeur opaque : tout le pipeline est ici, en C++/D3D11, lisible.

> Rappel honnête : ça ne transforme pas GTA 6 PS5 en version PC. Le jeu tourne toujours sur la
> console à 30 fps ; on rend l'image plus nette et le mouvement plus fluide **à l'écran**, pas la
> réactivité. L'interpolation ajoute environ un intervalle de frame de latence (~35-40 ms pour un jeu
> à 30 fps) en plus de la remote play. Bien pour l'exploration en solo, à couper pour le combat.

## Ce que ça fait

```
fenêtre Remote Play ──WGC──► recadrage zone client 16:9
   │
   ├─► pyramide de luminance ──► doublon ? (flux 60 fps, jeu 30 fps : 1 frame sur 2 répétée)
   │                                 │ non
   │                                 ▼
   │                  upscale (RTX VSR / bicubique) ──► historique des frames uniques
   │                  flow aller + retour (blocs 8x8, grossier→fin, propagation, sous-pixel)
   ▼
à chaque rafraîchissement de l'écran : image synthétisée à l'instant (maintenant − retard)
   + overlay ImGui (stats, réglages)
```

- **Capture** : Windows Graphics Capture d'une fenêtre. Les pixels ne passent jamais par le CPU,
  mais il y a des copies GPU : recadrage de la zone client (`CopySubresourceRegion`), puis copie de
  la capture le temps que sa mesure de doublon soit lue. Seuls ~330 octets de mesures (doublon,
  coupure) remontent au CPU par capture, via deux tampons de lecture alternés : la mesure est lue au
  rendu suivant sans attendre le GPU, sauf si l'image à afficher en dépend (passthrough, frame
  attendue en retard) ; le nombre de ces lectures bloquantes est affiché dans l'overlay. Fonctionne
  même si la fenêtre est cachée derrière l'overlay (pas si elle est minimisée).
- **Dédoublonnage** : différence moyenne par tuile sur la luminance ¼ de résolution. Par défaut le
  seuil est **adaptatif** : sur les 64 dernières captures, les répétitions (bruit de réencodage) et
  les vraies images forment deux populations ; le seuil glisse entre les deux (borné entre ¼ et 4×
  `--dup-threshold`) et revient à la base quand il n'y a qu'une population (contenu 60 fps, image
  fixe). Un **verrou de rythme** reconnaît le schéma A A B B (jeu à 30 fps dans un flux à 60, ou 1
  sur 3) et ne tranche que les mesures ambiguës. `--no-adaptive-dedupe` rétablit le seuil fixe. Les
  doublons ne sont ni upscalés ni interpolés.
- **Upscale**
  - `vsr` : **NVIDIA RTX Video Super Resolution** via le VideoProcessor D3D11 (la même voie que
    Chrome/VLC/mpv), pas de SDK à télécharger. Il faut activer *Super Resolution* dans
    **NVIDIA App > Système > Vidéo** (qualité 3 ou 4 conseillée), sinon le driver fait un simple
    scaling.
  - `bicubic` : Catmull-Rom + netteté bornée (pas de halo), tous GPU.
- **Interpolation** (maison, compute shaders) : estimation de mouvement par blocs sur 5 niveaux de
  pyramide, propagation des vecteurs entre voisins, médiane, flow bidirectionnel, synthèse avec
  gestion des occlusions et des éléments fixes (HUD, sous-titres). Elle cale l'affichage sur le
  rafraîchissement de **ton** écran (60, 144, 240 Hz...), pas sur un multiplicateur fixe.
- **HUD / texte fixe** : masque de bords (Sobel 3×3 sur la luminance demi-résolution, dilaté 3×3)
  calculé une fois par image unique. Près d'un bord net, la synthèse tolère une différence A/B plus
  grande (bruit de compression autour des traits) pour recopier le pixel fixe au lieu de
  l'interpoler, pondérée par la stabilité entre A et B. Ailleurs, critère d'origine inchangé.
  `--no-hud-protect` pour le désactiver.
- **Changements de plan** : en plus de la différence moyenne (critère d'origine), corrélation de la
  luminance par blocs de ~32 px (meilleure de 9 décalages d'un bloc, pour ignorer le mouvement de
  caméra) et distance d'histogrammes, calculées sur le niveau le plus grossier de la pyramide. La
  corrélation ne dépend pas de la luminosité : elle repère les coupures entre deux plans sombres.
  Sur une coupure, pas d'interpolation : l'image précédente est tenue (**HOLD**) jusqu'à l'instant
  de la nouvelle. Un panoramique rapide sur un décor contrasté dépasse la différence moyenne sans
  changer de plan : si la corrélation reste haute (≥ 0,85) et les histogrammes proches (≤ 0,35), la
  différence moyenne est ignorée. `--simple-cuts` revient au seul critère d'origine.
- **Overlay** : fenêtre plein écran au-dessus de la remote play, qui ne prend **jamais le focus**.
  La manette reste donc branchée sur la remote play. Il est exclu des captures d'écran
  (`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`, Windows 10 2004+) : un enregistreur ou un
  partage d'écran voit la remote play d'origine, et une capture d'écran entier ne peut pas
  reboucler sur l'overlay (la capture WGC de la fenêtre remote play ne l'incluait déjà pas).
  `--capturable` pour streamer l'image améliorée (OBS en capture d'écran).

## Ce qui a été corrigé par rapport à la phase 1 d'origine

- Le contexte D3D11 était utilisé depuis le thread de capture WGC **et** le thread de rendu en même
  temps (crash ou corruption). Tout le GPU tourne maintenant sur un seul thread.
- `CopyResource` vers le back buffer exigeait la même taille que la capture : écran noir dès que la
  fenêtre n'avait pas exactement la taille de la remote play. Remplacé par un rendu mis à l'échelle
  avec bandes noires.
- Le « hash » par 256 `CopySubresourceRegion` + `Map` bloquant à chaque frame est remplacé par une
  mesure GPU, plus fiable contre le bruit de compression, lue sans blocage quand c'est possible.
- Une texture était allouée à chaque frame ; le redimensionnement de la fenêtre capturée cassait la
  capture ; la barre de titre était capturée ; les guillemets dans la ligne de commande empêchaient de
  trouver la fenêtre ; `ResizeBuffers` échouait avec la RTV encore liée ; en-têtes manquants
  (`<cstdio>`, `<stdexcept>`) et ambiguïté `Windows::` avec C++/WinRT.
- Dans la phase 1, la fenêtre rpenhance prenait le focus : la remote play perdait la manette.

> Pour compiler et valider sur Windows pas à pas : voir [docs/GUIDE-TEST-WINDOWS.md](docs/GUIDE-TEST-WINDOWS.md).

## Compiler

Prérequis : Visual Studio 2022 ou 2026 avec la charge **Développement Desktop en C++** (MSVC,
Windows SDK, CMake inclus). ImGui est téléchargé automatiquement.

```powershell
.\build.ps1
```

L'exécutable est dans `build\Release\rpenhance.exe`.

Intégration continue : [`.github/workflows/windows-build.yml`](.github/workflows/windows-build.yml)
compile avec MSVC sur `windows-latest` (sans SDK NVIDIA), lance `ctest` puis
`rpenhance.exe --selftest --warp` : le selftest tourne sur WARP, le rendu logiciel D3D11, car le
runner n'a pas de GPU. Cette étape ne bloque pas encore le job ; `rpenhance.log` et les images
sont dans l'artefact `selftest-warp`, les PSNR dans le résumé du run. Sur WARP, les temps GPU ne
veulent rien dire. Le test du frame tap (`tap_writer` + `--selftest-tap --warp`) est en revanche
bloquant ; ses journaux sont dans `tap-logs/` du même artefact.

## Utiliser

1. Lance **PS Remote Play** (ou chiaki), mets la fenêtre en **plein écran / 1080p** : la capture prend
   la taille de la fenêtre, donc une petite fenêtre = une image floue avant même l'upscale.
2. Dans la remote play, choisis 1080p et 60 fps (Paramètres > Qualité vidéo).
3. Lance `rpenhance.exe`. Il trouve tout seul « PS Remote Play » ou « chiaki », sinon passe une partie
   du titre : `rpenhance.exe "Remote Play"`. `--list` affiche les fenêtres disponibles.
4. Si la manette ne répond plus, fais Alt+Tab vers la remote play (l'overlay reste au-dessus).

Raccourcis globaux : **Ctrl+Alt+S** stats, **U** upscale, **F** frame generation, **C** comparaison
avant/après, **V** visualisation du flow, **Q** quitter.

Options : `rpenhance.exe --help`. Les plus utiles :

| Option | Effet |
|---|---|
| `--upscale off\|bicubic\|vsr\|nvvfx` | méthode d'upscale (défaut `vsr`) ; `nvvfx` : voir ci-dessous |
| `--interp flow\|nvvfx` | interpolation maison (défaut) ou VFG du SDK NVIDIA, voir ci-dessous |
| `--no-fg` | pas d'interpolation : latence minimale, VRR/tearing autorisé |
| `--dup-threshold 1.2` | seuil de base des doublons (seuil fixe avec `--no-adaptive-dedupe`) |
| `--no-adaptive-dedupe` | désactive le seuil adaptatif et le verrou de rythme |
| `--simple-cuts` | coupures détectées sur la seule différence moyenne |
| `--no-hud-protect` | pas de masque de bords pour garder le HUD fixe |
| `--delay 1.15` | marge du retard ; à monter si l'image saccade quand le réseau varie |
| `--flow-scale 0.75` | flow estimé à résolution réduite (0.5 à 1.0, défaut 1.0) : moins de GPU, vecteurs plus grossiers ; synthèse toujours en pleine résolution |
| `--windowed` | fenêtre classique au lieu de l'overlay |
| `--monitor 1` | overlay sur un autre écran |
| `--capturable` | l'overlay reste visible des captures d'écran (OBS, partage) |
| `--source wgc\|tap\|capture` | fenêtre Remote Play (défaut), frame tap, ou carte de capture HDMI (voir « Deux modes ») |

Le journal est écrit dans `rpenhance.log` à côté de l'exe (stats toutes les 5 s).

## Source « frame tap » (expérimental)

Au lieu de capturer la fenêtre de la remote play (WGC, qui passe par le compositeur DWM et ajoute sa
latence), rpenhance peut lire des frames **déjà décodées** qu'un autre programme dépose dans une
mémoire partagée, au format « frame tap » **version 2** de SmoovPlay (spécification MIT :
[docs/frame-tap.md de SmoovPlay](https://github.com/smoovplay/smoovplay/blob/main/docs/frame-tap.md)).

```powershell
.\build\Release\rpenhance.exe --source tap                       # mémoire Local\smoovplay-tap
.\build\Release\rpenhance.exe --source tap --tap-name Local\autre  # autre nom
```

| Option | Effet |
|---|---|
| `--source wgc\|tap` | `wgc` (défaut, inchangé) : capture de fenêtre ; `tap` : frame tap |
| `--tap-name NOM` | nom de la mémoire partagée (défaut `Local\smoovplay-tap`) ; l'event est `NOM-frame` |
| `--tap-trust-flags` | le pipeline croit les métadonnées de l'émetteur (désactivé par défaut, voir plus bas) |
| `--selftest-tap` | contrôle automatique avec `tap_writer` (voir « Tester sans PS5 ») |

Ce qui est lu :

- mémoire `CreateFileMappingW` de 4096 + 64 Mio créée ou ouverte (l'émetteur peut démarrer avant ou
  après), event auto-reset `NOM-frame` attendu par la boucle (pas d'attente active) ;
- en-tête `CFTP`, **version 2 obligatoire** : une version 1 (frames compressées H.264/H.265) est
  refusée avec un message explicite dans le journal ; `session` suivie (nouveau flux = suivi remis à
  zéro) ; `reader_heartbeat` écrit à chaque lecture ;
- NV12 ou BGRA, lignes tassées. NV12 : Y et Cb/Cr envoyés au GPU (R8 / R8G8) puis convertis en BGRA
  par shader, matrice selon `colour` (0 auto : BT.709 à partir de 720 lignes, BT.601 en dessous,
  plage limitée ; 1 à 4 explicites), chrominance 4:2:0 interpolée (placement H.264 par défaut) ;
- lecture sans jamais bloquer l'émetteur : la frame la plus récente seulement (les autres sont
  sautées si rpenhance est en retard), `seq` du slot relu après la copie et frame abandonnée si le
  slot a été réécrit entre-temps ;
- `qpc_arrival` converti en secondes sur la même horloge QPC que le reste du pipeline : le retard
  d'arrivée mesuré part de l'arrivée de la frame chez l'émetteur.

Métadonnées des slots (`flags` : bit 0 tronquée, bit 1 répétition, bit 2 nouveau contenu ;
`frames_lost` ; `recovered`) : toujours journalisées/exposées par la source, mais **utilisées par le
pipeline seulement avec `--tap-trust-flags`** : répétition = doublon sans mesure ; nouveau contenu =
image unique sans passer par le classement des doublons ; `frames_lost > 0` ou frame tronquée = pas
d'interpolation vers ni depuis cette frame (l'image est tenue) ; `recovered` ou nouvelle session =
historique vidé. Sans l'option, le pipeline se comporte exactement comme avec WGC.

Limites :

- **chiaki-ng émet aujourd'hui des frames compressées (version 1)** via son patch frame tap (AGPL) :
  rpenhance ne les décode pas et refuse ce flux. La version 2 suppose un **autre émetteur** qui
  dépose des frames décodées (aucun n'existe pour PS Remote Play à ce jour).
- **Aucune frame réelle n'a été testée** : seulement l'émetteur synthétique `tests/tap_writer.cpp`.
- Feature 8 (frames envoyées seulement quand l'image change) : pas de recalage dédié sur 60 Hz, les
  frames sont traitées à leur arrivée. Pas de son (l'anneau `-audio` est ignoré), pas de HDR.

## Backend optionnel : NVIDIA Video Effects SDK (VFG / VSR)

Expérimental, **désactivé par défaut**, jamais testé à ce jour (écrit sans MSVC ni GPU, voir
« Limites connues »). Il remplace, quand il est disponible :

- l'interpolation maison par **Video Frame Generation** (VFG, filtre IA du NVIDIA Video Effects SDK)
  avec `--interp nvvfx` : image générée à l'instant exact voulu entre les deux frames uniques qui
  encadrent l'affichage (mode « timestep explicite »), jamais à travers une coupure (HOLD conservé) ;
- l'upscale par la **Video Super Resolution** du SDK avec `--upscale nvvfx` (`--nvvfx-vsr-mode 0..4`,
  défaut 3 = VSR_High).

**Repli** : exe compilé sans l'option, SDK ou feature absents, DLL manquante, GPU non supporté, pilote
trop ancien, échec de chargement ou d'exécution → message unique dans `rpenhance.log` et retour au
pipeline actuel (flow maison ; RTX VSR du VideoProcessor puis bicubique). L'overlay affiche la voie
réellement utilisée.

**Exigences** (guide NVIDIA VFX SDK 1.3) : Windows 10/11 x64, pilote NVIDIA **≥ 570.65** ; VFG
uniquement sur GPU **Ada (RTX 40) ou Blackwell (RTX 50)** ; VSR sur Turing et plus récents.

**Licence** : le SDK (DLL, en-têtes, modèles) appartient à NVIDIA et n'est **pas fourni** avec
rpenhance ni dans ce dépôt. D'après le catalogue NGC, il est régi par le *NVIDIA Software License
Agreement* et les *Product-Specific Terms for NVIDIA AI Products*, les modèles par la licence indiquée
pour chacun. Lis-les avant usage. **Avant de publier des mesures de VFG** (PSNR du selftest, temps
GPU, comparaisons), vérifie que la licence en vigueur l'autorise : d'anciennes licences Maxine
traitaient les résultats de benchmark comme informations confidentielles.

Installation et compilation :

```powershell
# 1. Clé NGC personnelle (ngc.nvidia.com > Setup > Generate Personal Key), seulement en variable
#    d'environnement de la session : jamais en argument, jamais dans un fichier versionné.
$env:NGC_API_KEY = Read-Host -MaskInput 'Clé NGC'          # PowerShell 7
# 2. SDK Core + features VSR et VFG dans third_party\nvvfx-sdk (ignoré par git)
.\scripts\install-nvvfx.ps1 -Gpu 120                        # 120 = Blackwell, 89 = Ada
Remove-Item Env:NGC_API_KEY
# 3. Compilation avec l'option (lit seulement les en-têtes du SDK installé, ne lie aucune lib NVIDIA)
cmake -S . -B build -A x64 -DRPENHANCE_NVVFX=ON              # -DNVVFX_SDK_DIR=<racine> si ailleurs
cmake --build build --config Release
.\build\Release\rpenhance.exe --interp nvvfx --upscale nvvfx
```

Le SDK est chargé dynamiquement (`LoadLibrary`) au premier usage, depuis `--nvvfx-dir`, puis
`NVVFX_SDK_DIR`, `VFXSDK_ROOT`, `<exe>\nvvfx-sdk` et enfin le dossier donné à CMake. Le premier
chargement d'un modèle peut prendre plusieurs secondes (l'image se fige pendant ce temps).

Interop D3D11 ↔ CUDA : chaque texture est enregistrée une fois (`NvCVImage_InitFromD3DTexture`),
puis mappée sur le flux CUDA du SDK, copiée GPU→GPU (`NvCVImage_Transfer`) et démappée. Les pixels
ne passent jamais par le CPU, mais ce n'est pas du zéro copie strict : les filtres n'acceptent que de
la mémoire CUDA linéaire, d'où une copie GPU par entrée (réutilisée : B d'une paire devient A de la
suivante) et une par image générée. La synchronisation repose sur les garanties de
`cudaGraphicsMapResources/UnmapResources` (le travail D3D11 émis avant le map précède le travail CUDA,
le travail CUDA émis avant l'unmap précède le travail D3D11 suivant), sans attente CPU, et tout reste
sur le thread GPU unique de l'application. Détails dans `src/nvvfx/nvvfx_backend.cpp`.

Coût : VFG tourne en taille de sortie et une fois par image affichée qui tombe entre deux frames
(jusqu'au taux de rafraîchissement de l'écran). NVIDIA annonce ~1,3 à 1,9 ms en 720p sur RTX
4090/5090 (mode Medium) ; en 1440p à 144 Hz le budget peut ne pas tenir : `--vfg-mode low` ou une
fréquence d'écran plus basse. Le temps GPU de VFG (flux CUDA) n'apparaît pas dans les mesures GPU de
l'overlay.

## Deux modes : Remote Play et carte de capture HDMI

rpenhance applique le même pipeline (doublons, coupures, upscale, interpolation) à deux sources :

```
Mode Remote Play (défaut, --source wgc ; ou --source tap)
  PS5 ──réseau──► PS Remote Play / chiaki (décodage) ──WGC──► recadrage 16:9 ──┐
                                                                               ├─► pipeline ──► écran
Mode carte de capture (--source capture, alias --hdmi)                         │
  PS5 ──HDMI──► carte de capture ──USB──► Media Foundation ──► NV12/RGB32 ──► BGRA (VideoProcessor)
                    │                  └─► entrée audio ──► relais WASAPI (retardé comme l'image) ──► haut-parleurs
                    └──HDMI (recopie, optionnel) ──► TV / écran
```

| | Remote Play (`wgc`, défaut) | Carte de capture (`capture`) |
|---|---|---|
| Matériel | aucun | carte de capture HDMI vue par Windows comme une caméra (UVC), câble HDMI |
| Manette | sur le PC (la remote play garde le focus, rpenhance est un overlay passif) | sur la console (rpenhance est une fenêtre plein écran qui prend le focus, Échap quitte) |
| Image reçue | flux vidéo compressé, décodé par la remote play, bruit de réencodage | sortie HDMI de la console, sans réseau ni réencodage (sauf cartes MJPEG) |
| Doublons | seuil de base 1.2 (bruit de réencodage) | seuil de base 0.4 (répétitions identiques au pixel) ; le seuil adaptatif et le verrou de rythme restent actifs |
| Recadrage | zone client recadrée en 16:9 (`--no-crop` pour l'éviter) | jamais : l'image HDMI est déjà la sortie de la console ; bandes noires seulement si l'écran n'est pas au même format |
| Son | celui de la remote play | relayé depuis l'entrée audio de la carte, retardé pour rester synchrone (`--audio-delay`) |
| Latence | réseau + encodage/décodage de la remote play + capture WGC (compositeur DWM) + interpolation | latence propre de la carte (USB, éventuel MJPEG) + interpolation ; pas de réseau ni de remote play |

Aucun chiffre de latence n'a été mesuré pour l'un ou l'autre mode. L'overlay affiche en direct le
retard d'arrivée des images (« arrivée » : horodatage de capture → réception) et le retard imposé par
l'interpolation (« retard FG ») : c'est ce qu'il faut comparer entre les deux modes (voir
[docs/GUIDE-TEST-WINDOWS.md](docs/GUIDE-TEST-WINDOWS.md), « Carte de capture HDMI »). Dans les deux
modes, l'interpolation ajoute environ un intervalle entre images uniques ; `--no-fg` la coupe.

### Utiliser une carte de capture

1. Sur la PS5 : **Paramètres > Système > HDMI > Activer HDCP : désactivé**, sinon la carte reçoit
   une image noire.
2. Branche la carte (USB 3 de préférence) et vérifie qu'elle apparaît : `rpenhance.exe --list-devices`
   (cartes vidéo et entrées audio). `rpenhance.exe --list-devices --source capture` (ou
   `--device "nom"`) liste aussi les **modes natifs** de la carte retenue.
3. Lance `rpenhance.exe --source capture` (ou `--hdmi`). La carte est détectée par son nom (Elgato,
   AVerMedia, « capture », « HDMI », « Cam Link », « Live Gamer », Magewell, « USB Video », UVC),
   sinon `--device "nom"`. Le meilleur mode est choisi automatiquement (≥ 60 fps, puis la plus grande
   résolution jusqu'à la 4K, puis non compressé plutôt que MJPEG) ; pour forcer : `--mode 1920x1080@60`
   ou `--mode 1280x720` (n'importe quelle fréquence). La liste des modes est aussi dans `rpenhance.log`.

| Option | Effet |
|---|---|
| `--source capture` / `--hdmi` | carte de capture au lieu de la fenêtre Remote Play |
| `--device "nom"` | sous-chaîne du nom de la carte (implique `--source capture`) |
| `--mode WxH[@fps]` | mode natif à utiliser (implique `--source capture`) |
| `--capture-sync` | lecture synchrone de la carte sur un thread dédié (ancien mode ; repli si la lecture asynchrone par défaut pose problème) |
| `--list-devices` | cartes vidéo, entrées audio, et modes de la carte retenue avec `--source capture`/`--device` |
| `--audio-device "nom"` | entrée audio de la carte (défaut : déduite du nom de la carte vidéo) |
| `--audio-volume 0.8` | volume du son relayé (aussi dans l'overlay) |
| `--audio-delay auto\|<ms>\|off` | retard du son relayé (défaut `auto`, voir ci-dessous) |
| `--no-audio` | pas de relais (si tu écoutes via la recopie HDMI ou la prise casque de la carte) |
| `--source capture --test-pattern` | mire au profil « câble », sans carte (voir « Tester sans PS5 ») |

Ce que fait la source (`src/capture/mf_capture.cpp`) :

- Media Foundation Source Reader en faible latence, avec le device D3D11 de rpenhance : la carte (ou
  la conversion de Media Foundation) livre si possible des textures GPU (zéro copie), sinon une copie
  depuis la mémoire système. Le format natif (NV12, YUY2, MJPEG, P010, H.264...) est converti en NV12
  par Media Foundation ; repli sur RGB32 si le NV12 est refusé ou si le natif est déjà RGB. Le
  VideoProcessor D3D11 produit ensuite la texture BGRA attendue par le pipeline (BT.601/709 et plage
  limitée/pleine lues dans le type média ; à défaut BT.601 sous 720 lignes, BT.709 au-delà).
- La lecture tourne sur un thread dédié qui ne garde que l'image la plus récente et réveille la boucle
  par un event : ni le thread GPU ni l'interface n'attendent la carte.
- Horodatage : instant de capture converti sur l'horloge du pipeline (QPC). Si le pilote fournit
  `MFSampleExtension_DeviceReferenceSystemTime`, il est repris tel quel ; sinon l'horodatage Media
  Foundation (unités de 100 ns) est recalé sur l'arrivée (plus petit décalage observé, qui suit
  lentement une dérive d'horloge) pour effacer la gigue de livraison USB. La base utilisée est écrite
  dans le journal. Le retard d'interpolation se cale ainsi sur l'arrivée réelle, comme en Remote Play.
- Un jeu à 30 fps dans un signal HDMI à 60 Hz se traite comme en Remote Play : une image sur deux est
  une répétition, reconnue par le dédoublonnage (rythme 2 verrouillé), et les coupures de plan restent
  détectées de la même façon (`DupClassifier` et `scene_cut` ne changent pas).
- Erreurs : pas de carte, nom introuvable, mode absent, format non convertible ou API vidéo D3D11
  indisponible → message clair (boîte de dialogue + journal), sans crash. Carte débranchée en cours de
  route → rpenhance s'arrête proprement avec un message dans le journal.

### Pourquoi le son est retardé (`--audio-delay`)

Avec la frame generation, l'image affichée n'est pas la plus récente : c'est celle de l'instant
(maintenant − retard), le temps de disposer de l'image suivante pour interpoler entre les deux. Le son de
la carte, s'il était relayé tel quel, sortirait donc **en avance sur l'image** d'autant. Le relais
WASAPI garde alors les échantillons dans une file le temps voulu :

- `auto` (défaut) : retard = « retard FG » − « arrivée » de l'overlay, c'est-à-dire ce que
  l'interpolation ajoute par rapport au passthrough (le retard d'arrivée USB/pilote touche aussi le son de
  la même carte, il n'est pas ajouté). 0 quand la frame generation est coupée (Ctrl+Alt+F suivi en
  direct). Le retard ne bouge que par sauts d'au moins 8 ms, au plus toutes les 0,5 s : chaque changement
  s'entend comme un petit trou (retard qui augmente) ou un petit saut (retard qui diminue).
- `--audio-delay 40` : retard fixe en ms (0 à 250), si l'image et le son ne sont pas d'accord chez toi
  (les latences propres de la sortie audio, de l'écran et de la carte ne sont pas mesurées).
- `--audio-delay off` : comportement d'origine (son relayé sans retard).

Hors retard voulu, la politique d'origine reste : jamais plus de 60 ms d'avance accumulée (l'excédent
est jeté pour ne pas dériver). Le retard appliqué est affiché dans l'overlay et dans le journal.

Toute carte UVC (vue par Windows comme une webcam) devrait fonctionner, c'est-à-dire la plupart des
cartes USB récentes. D'après la branche `hdmi-capture` d'origine, image et son ont été validés avec une
webcam USB et son micro (mêmes pilotes UVC et audio USB), pas avec une vraie carte HDMI ni une PS5. Les
adaptations de la branche `feat/hdmi-merge` (interface `FrameSource`, horodatage, repli RGB32, retard du
son) n'ont été vérifiées que hors Windows (voir « Limites connues »). La copie depuis la mémoire système
(cartes qui ne livrent pas d'images sur le GPU) n'a jamais été testée.

## Tester sans PS5

```powershell
.\build\Release\rpenhance.exe --test-pattern --windowed
```

Mire synthétique qui imite la remote play : jeu à 30 fps dans un flux à 60 fps, gigue réseau, bruit
de réencodage, HUD fixe.

Variante « carte de capture » sans carte : `--source capture --test-pattern` (jeu à 30 fps dans un
signal HDMI à 60 Hz, répétitions identiques au pixel, horodatages réguliers, fenêtre plein écran qui
prend le focus comme avec une vraie carte ; pas de son).

```powershell
.\build\Release\rpenhance.exe --source capture --test-pattern
```

```powershell
.\build\Release\rpenhance.exe --selftest; Get-Content .\build\Release\rpenhance.log
```

Compare l'image interpolée à la vraie image intermédiaire de la mire et écrit les images dans
`build\Release\selftest\`. Résultats mesurés sur RTX 5070 (driver 617.42), image du milieu en 1440p :

| Mouvement | Répéter la frame | Fondu | Interpolation rpenhance |
|---|---|---|---|
| normal (pan 14 px/frame) | 16,1 dB | 18,8 dB | **26,5 dB** |
| rapide (×2,5) | 12,9 dB | 15,5 dB | **19,7 dB** |

Coût GPU : ~2,6 ms par frame unique (flow aller + retour depuis une source 1080p), 0,1 ms par image
affichée. Ces chiffres datent d'avant la branche `feat/robustesse-qualite` (mesures de coupure,
masque de bords, `--flow-scale`) : à remesurer. Le selftest journalise aussi la qualité avec et sans
protection du HUD et avec `--flow-scale 0.5`.

Frame tap sans PS5 : `tap_writer` (émetteur de test, copie adaptée de l'émetteur MIT de SmoovPlay)
envoie deux sessions de frames NV12 ou BGRA, avec arrêt puis redémarrage ; `--selftest-tap` les lit,
relit chaque image convertie et vérifie numéro de frame, couleurs, drapeaux et sessions (code 0/1).

```powershell
Start-Process .\build\Release\rpenhance.exe '--selftest-tap','--tap-name','Local\rpenhance-tap-test'
.\build\Release\tap_writer.exe --format nv12 --name Local\rpenhance-tap-test   # ou --format bgra
Get-Content .\build\Release\rpenhance.log   # « SELFTEST TAP OK » attendu
```

La CI Windows lance ce test sur WARP (NV12 720p, NV12 480p, BGRA 720p) et échoue s'il ne passe pas.

## Limites connues

- Carte de capture (`--source capture`, branche `feat/hdmi-merge`) : intégration de la branche
  `hdmi-capture` adaptée à l'interface `FrameSource`, écrite sans MSVC, sans carte de capture et sans
  PS5. Vérifié : syntaxe avec clang/llvm-mingw (en-têtes Windows de mingw-w64, pas le SDK MSVC) et la
  logique pure (`tests/capture_logic_test.cpp`, `tests/audio_delay_test.cpp`, aussi lancés par ctest
  dans la CI). Non vérifiés : compilation MSVC (la CI le dira), présence réelle de
  `MFSampleExtension_DeviceReferenceSystemTime` selon les pilotes, repli RGB32, copie depuis la mémoire
  système, synchronisation image/son à l'oreille, latence comparée à la Remote Play. Pas de HDR (P010
  converti en NV12 8 bits), pas de désentrelacement (1080i).

- Les modifications de la branche `feat/robustesse-qualite` (seuil adaptatif, coupures, HUD, lecture
  GPU asynchrone, `--flow-scale`...) ont été écrites sans compilateur MSVC ni GPU : seules la syntaxe
  (g++ avec en-têtes Windows factices, dxc pour le HLSL) et la logique CPU du dédoublonnage
  (`tests/dedupe_test.cpp`) ont été vérifiées. À compiler et valider sur Windows (`--selftest`,
  `--test-pattern`) avant usage.

- `--flow-scale` < 1 : moins de travail pour le flow (environ proportionnel au carré de l'échelle),
  mais blocs de mouvement plus gros en pixels réels, donc contours d'objets plus baveux ; la mesure
  de doublon et le masque de bords passent aussi à la résolution réduite (le seuil adaptatif suit).
  Changer l'échelle dans l'overlay vide l'historique (une courte saccade).
- Protection du HUD : un HUD semi-transparent sur un décor qui bouge n'est pas « fixe » (A et B
  diffèrent) et reste interpolé ; un bord d'objet qui bouge de moins d'un demi-pixel peut être tenu en
  fondu au lieu d'être déplacé (invisible en pratique, mais non mesuré sur du vrai contenu).
- Détection de coupure : une caméra très rapide (plus d'environ 50 px entre deux images uniques en
  1080p) ou deux plans de disposition semblable peuvent tromper la corrélation ; dans le premier cas
  on tient l'image au lieu d'interpoler (saccade ponctuelle), dans le second l'interpolation passe
  comme avant. Seuils ajustables dans l'overlay (« Changements de plan »).
- La latence : voir l'avertissement en haut. Coupe la frame generation (Ctrl+Alt+F) pour viser.
- L'interpolation par blocs bave sur les rotations rapides et les objets très fins ; les zones en
  désaccord retombent sur la frame la plus proche plutôt que de produire un fantôme.
- Impossible de vérifier depuis l'appli que VSR est réellement appliqué : le driver accepte
  l'extension, mais c'est le réglage de NVIDIA App qui décide. Compare avec Ctrl+Alt+C.
- Pas de HDR.
- Backend NVIDIA VFX (`--interp nvvfx`, `--upscale nvvfx`) : écrit sans MSVC, sans SDK installé et sans
  GPU Ada/Blackwell. Vérifié seulement : syntaxe g++ avec des en-têtes Windows et SDK factices, absence
  de liaison statique au SDK, configuration CMake de l'option, analyse du script PowerShell. Non
  vérifiés : noms exacts des sélecteurs dans les en-têtes du SDK 1.3, comportement de
  `NvCVImage_InitFromD3DTexture`/`Transfer` sur des textures BGRA de rpenhance, synchronisation réelle,
  qualité et coût de VFG sur le flux Remote Play, URL de l'API NGC et structure de l'archive du SDK.

- Branche `perf/flow` (optimisation de l'estimation de mouvement : SAD en `GatherRed`, flow aller +
  retour dans les mêmes Dispatch) : écrite sans MSVC ni GPU, syntaxe HLSL vérifiée avec dxc en profil
  6_0 seulement (l'appli compile en 5_0 avec FXC). Gain non mesuré : 128 → 32 lectures de texture par
  vecteur candidat et 34 → 17 Dispatch de flow par frame unique sont des comptes faits en lisant le
  code. Comparaison à faire : [docs/GUIDE-TEST-WINDOWS.md](docs/GUIDE-TEST-WINDOWS.md), étape 7.

## Pistes pour la suite

- Modèle RIFE (ONNX Runtime + DirectML ou TensorRT) à la place de l'estimateur par blocs, avec la
  même mire pour mesurer le gain.
- Un émetteur de frames décodées (frame tap v2) côté client remote play, pour profiter de la source
  `--source tap` (moins de latence que WGC) ; aujourd'hui seul l'émetteur de test existe.
- Rendre et présenter avant le travail lourd d'une nouvelle frame unique (pour ne pas rater un vblank à
  144/240 Hz) : pas fait sans mesure. Le travail lourd (upscale + flow) part souvent de `render()`
  lui-même (`resolve_pending`), pas seulement de `submit()` ; inverser l'ordre dans la boucle ajouterait
  une image de latence en passthrough, retarderait l'arrivée de B en FG (plus d'images tenues faute de B,
  retard d'arrivée mesuré différemment). À reprendre avec des mesures PresentMon sur Windows (par exemple : soumettre au GPU la
  composition avant le flow, dans `Pipeline`).

## Licence

rpenhance est distribué sous licence **MIT** (voir [`LICENSE`](LICENSE)). Dear ImGui (MIT, Omar
Cornut), téléchargé à la compilation, et les projets dont des idées ont été reprises sont listés
dans [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). Le NVIDIA Video Effects SDK, optionnel, n'est
pas inclus et reste sous licence NVIDIA (voir la section dédiée ci-dessus).
