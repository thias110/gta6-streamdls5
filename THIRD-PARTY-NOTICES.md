# Notices tierces

rpenhance est distribué sous licence MIT (voir `LICENSE`). Il s'appuie sur les composants et
travaux suivants.

## Dear ImGui v1.91.5

Bibliothèque d'interface (overlay de stats et de réglages). Elle n'est pas incluse dans ce dépôt :
CMake la télécharge à la compilation (`FetchContent`, tag `v1.91.5`) et la lie statiquement à
l'exécutable. Source : https://github.com/ocornut/imgui

```
The MIT License (MIT)

Copyright (c) 2014-2024 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## NVIDIA Video Effects SDK (optionnel, non inclus)

Backend facultatif (`-DRPENHANCE_NVVFX=ON`, `--interp nvvfx`, `--upscale nvvfx`). **Aucun fichier
NVIDIA n'est inclus dans ce dépôt ni dans les binaires de rpenhance** : ni DLL, ni modèle, ni en-tête.

- Le VFX SDK Core et ses features (Video Super Resolution, Video Frame Generation) sont téléchargés
  par l'utilisateur depuis NVIDIA NGC (`scripts/install-nvvfx.ps1`, avec sa propre clé NGC) dans
  `third_party/nvvfx-sdk/`, ignoré par git. D'après le catalogue NGC, leur usage est régi par le
  *NVIDIA Software License Agreement* et les *Product-Specific Terms for NVIDIA AI Products* ; les
  modèles par la licence indiquée pour chacun (*NVIDIA Community Model License* ou *NVIDIA Open
  Model License*). Ces licences ne sont pas des licences open source : ne pas redistribuer ces
  fichiers avec rpenhance.
- À la compilation, seuls les en-têtes du SDK installé localement sont lus ; aucune bibliothèque
  NVIDIA n'est liée. À l'exécution, les DLL sont chargées dynamiquement si elles sont présentes.
- Configuration requise selon NVIDIA (guide VFX SDK 1.3) : pilote Windows ≥ 570.65 ; Video Frame
  Generation uniquement sur GPU Ada ou Blackwell.
- Avant de publier des mesures de qualité ou de performance de Video Frame Generation, vérifier
  que la licence NVIDIA en vigueur le permet.
- Le code de `src/nvvfx/` est original (MIT, rpenhance). Il suit la documentation publique de NVIDIA
  (guide VFX SDK, guide NvCVImage) et l'ordre d'appels de l'exemple MIT
  `VideoFrameGenerationEffectApp` (https://github.com/NVIDIA-Maxine/VFX-SDK-Samples) ; aucun code
  n'en est copié. Le dépôt MIT https://github.com/NVIDIA-Maxine/Maxine-VFX-SDK (proxy
  `NVVideoEffectsProxy.cpp`) n'est ni copié ni téléchargé : ses en-têtes (v0.7.6) ne couvrent pas
  Video Frame Generation.

## SmoovPlay : format « frame tap » et émetteur de référence (MIT)

Source : https://github.com/smoovplay/smoovplay — licence MIT, Copyright (c) 2026 the SmoovPlay
authors.

- `src/capture/tap_protocol.{hpp,cpp}`, `src/capture/tap_source.{hpp,cpp}`, `src/selftest_tap.cpp` :
  lecteur du format « frame tap » version 2 écrit pour rpenhance d'après la spécification
  `docs/frame-tap.md` de SmoovPlay (disposition de la mémoire, en-tête, slots, sessions, drapeaux,
  couleur) et son émetteur de référence `docs/frame-tap-writer.c`.
- `tests/tap_writer.cpp` : **copie adaptée** de `docs/frame-tap-writer.c` (ouverture du tap, début de
  flux, écriture d'une frame, fin de flux, image de bandes mobiles), modifiée pour rpenhance (C++, NV12,
  plusieurs sessions, attente du lecteur, image de test `src/capture/tap_pattern.hpp`).
- Le lecteur de SmoovPlay (`framegen/tap.py`, `framegen/chiaki_tap.py`) n'a pas été repris. Le
  patch frame tap de chiaki-ng (`chiaki_tap/`, AGPL 3.0) n'a été ni lu ni copié.

```
MIT License

Copyright (c) 2026 the SmoovPlay authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Idées reprises (aucun code copié)

- **SmoovPlay** (https://github.com/smoovplay/smoovplay, licence MIT, Copyright (c) 2026 the
  SmoovPlay authors). Idées reprises :
  - `framegen/cadence.py` : seuil de doublon appris sur le bruit de fond des différences, verrou
    sur le rythme de période 2 ou 3 des doublons, détection de coupure par corrélation de la
    luminance par blocs et distance d'histogramme (utile entre deux plans sombres), mise en
    attente (HOLD) plutôt qu'interpolation à travers une coupure ;
  - `framegen/steady.py` : protection du HUD fixe, limitée aux pixels proches d'un bord (Sobel sur
    la luminance + dilatation) et pondérée par la stabilité entre les deux vraies frames.

  Le code C++/HLSL de rpenhance qui met ces idées en œuvre est une réimplémentation indépendante
  (structures, seuils et algorithmes de détail propres à rpenhance).

- **AMD FidelityFX Contrast Adaptive Sharpening (CAS)** (https://github.com/GPUOpen-Effects/FidelityFX-CAS,
  licence MIT, Copyright (c) 2019-2020 Advanced Micro Devices, Inc.). Idée reprise pour la netteté de
  `--enhance` (`ps_finish`, `src/gpu/shaders.hpp`) : moduler le gain de netteté selon le contraste local
  du voisinage 3x3 pour ne pas sur-accentuer les zones déjà contrastées. Aucun code ni aucune formule de
  CAS n'est copié : le filtre de rpenhance (moyenne pondérée 3x3, gain par `smoothstep` sur l'étendue
  locale, résultat borné au min/max du voisinage) est écrit pour rpenhance.
- Les autres traitements de `--enhance` sont des techniques classiques publiées (algorithmes seulement,
  aucun code repris), écrites pour rpenhance : filtre bilatéral (Tomasi et Manduchi, 1998), masque flou (« unsharp mask ») à grand rayon
  sur la luminance, courbe de tons polynomiale, dithering triangulaire (TPDF).

Aucun code de projets sous GPL ou AGPL (Veyra, chiaki-ng...) n'est inclus dans rpenhance.
