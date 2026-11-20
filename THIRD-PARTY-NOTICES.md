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

Aucun code de projets sous GPL ou AGPL (Veyra, chiaki-ng...) n'est inclus dans rpenhance.
