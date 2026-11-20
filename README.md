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

- **Capture** : Windows Graphics Capture d'une fenêtre, zéro copie CPU. Fonctionne même si la
  fenêtre est cachée derrière l'overlay (pas si elle est minimisée).
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
- **Overlay** : fenêtre plein écran au-dessus de la remote play, qui ne prend **jamais le focus**.
  La manette reste donc branchée sur la remote play.

## Ce qui a été corrigé par rapport à la phase 1 d'origine

- Le contexte D3D11 était utilisé depuis le thread de capture WGC **et** le thread de rendu en même
  temps (crash ou corruption). Tout le GPU tourne maintenant sur un seul thread.
- `CopyResource` vers le back buffer exigeait la même taille que la capture : écran noir dès que la
  fenêtre n'avait pas exactement la taille de la remote play. Remplacé par un rendu mis à l'échelle
  avec bandes noires.
- Le « hash » par 256 `CopySubresourceRegion` + `Map` bloquant à chaque frame est remplacé par une
  mesure GPU, plus fiable contre le bruit de compression.
- Une texture était allouée à chaque frame ; le redimensionnement de la fenêtre capturée cassait la
  capture ; la barre de titre était capturée ; les guillemets dans la ligne de commande empêchaient de
  trouver la fenêtre ; `ResizeBuffers` échouait avec la RTV encore liée ; en-têtes manquants
  (`<cstdio>`, `<stdexcept>`) et ambiguïté `Windows::` avec C++/WinRT.
- Dans la phase 1, la fenêtre rpenhance prenait le focus : la remote play perdait la manette.

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
| `--upscale off\|bicubic\|vsr` | méthode d'upscale (défaut `vsr`) |
| `--no-fg` | pas d'interpolation : latence minimale, VRR/tearing autorisé |
| `--dup-threshold 1.2` | seuil de base des doublons (seuil fixe avec `--no-adaptive-dedupe`) |
| `--no-adaptive-dedupe` | désactive le seuil adaptatif et le verrou de rythme |
| `--delay 1.15` | marge du retard ; à monter si l'image saccade quand le réseau varie |
| `--windowed` | fenêtre classique au lieu de l'overlay |
| `--monitor 1` | overlay sur un autre écran |

Le journal est écrit dans `rpenhance.log` à côté de l'exe (stats toutes les 5 s).

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
affichée.

## Limites connues

- Les modifications de la branche `feat/robustesse-qualite` (seuil adaptatif, coupures, HUD, lecture
  GPU asynchrone, `--flow-scale`...) ont été écrites sans compilateur MSVC ni GPU : seules la syntaxe
  (g++ avec en-têtes Windows factices, dxc pour le HLSL) et la logique CPU du dédoublonnage
  (`tests/dedupe_test.cpp`) ont été vérifiées. À compiler et valider sur Windows (`--selftest`,
  `--test-pattern`) avant usage.

- La latence : voir l'avertissement en haut. Coupe la frame generation (Ctrl+Alt+F) pour viser.
- L'interpolation par blocs bave sur les rotations rapides et les objets très fins ; les zones en
  désaccord retombent sur la frame la plus proche plutôt que de produire un fantôme.
- Impossible de vérifier depuis l'appli que VSR est réellement appliqué : le driver accepte
  l'extension, mais c'est le réglage de NVIDIA App qui décide. Compare avec Ctrl+Alt+C.
- Pas de HDR.

## Pistes pour la suite

- Modèle RIFE (ONNX Runtime + DirectML ou TensorRT) à la place de l'estimateur par blocs, avec la
  même mire pour mesurer le gain.
- Récupérer les frames décodées directement dans chiaki (moins de latence que WGC).

## Licence

rpenhance est distribué sous licence **MIT** (voir [`LICENSE`](LICENSE)). Dear ImGui (MIT, Omar
Cornut), téléchargé à la compilation, et les projets dont des idées ont été reprises sont listés
dans [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).
