# rpenhance

Upscale + interpolation d'images en temps réel sur le flux **PS Remote Play / chiaki** affiché sur PC.
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
  de la nouvelle. `--simple-cuts` revient au seul critère d'origine.
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

Le journal est écrit dans `rpenhance.log` à côté de l'exe (stats toutes les 5 s).

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

## Tester sans PS5

```powershell
.\build\Release\rpenhance.exe --test-pattern --windowed
```

Mire synthétique qui imite la remote play : jeu à 30 fps dans un flux à 60 fps, gigue réseau, bruit
de réencodage, HUD fixe.

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

## Limites connues

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

## Pistes pour la suite

- Modèle RIFE (ONNX Runtime + DirectML ou TensorRT) à la place de l'estimateur par blocs, avec la
  même mire pour mesurer le gain.
- Récupérer les frames décodées directement dans chiaki (moins de latence que WGC).

## Licence

rpenhance est distribué sous licence **MIT** (voir [`LICENSE`](LICENSE)). Dear ImGui (MIT, Omar
Cornut), téléchargé à la compilation, et les projets dont des idées ont été reprises sont listés
dans [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md). Le NVIDIA Video Effects SDK, optionnel, n'est
pas inclus et reste sous licence NVIDIA (voir la section dédiée ci-dessus).
