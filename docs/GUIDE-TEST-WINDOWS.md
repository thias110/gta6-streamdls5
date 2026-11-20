# Guide : compiler et tester rpenhance sous Windows

> **État honnête.** Le code des branches `fix/timing-grid`, `feat/robustesse-qualite` et
> `feat/nvidia-vfx` a été écrit et relu sans compilateur MSVC ni GPU : seule la syntaxe et la logique CPU
> (`tests/`) ont été vérifiées hors Windows. Ce guide sert à **valider pour de vrai**. Note chaque
> résultat, y compris les échecs : c'est ce qui permet de corriger.

Les étapes sont à faire dans l'ordre. Ne passe à la suivante que si la précédente marche.

## 0. Prérequis

- Windows 10 2004 ou plus récent (11 recommandé), carte NVIDIA RTX à jour (pilote **570.65 ou plus**
  pour le backend NVIDIA, voir étape 6).
- **Visual Studio 2022 ou 2026** avec la charge de travail **Développement Desktop en C++** (MSVC,
  Windows SDK, CMake inclus).
- **Git** et **PowerShell**.
- Pour l'usage réel : PS Remote Play (ou chiaki) et une PS5. Les étapes 1 à 4 marchent sans PS5.
- Pas besoin du CUDA Toolkit pour compiler (d'après la doc du SDK NVIDIA, non vérifié : si CMake ou le
  build en réclame un, note-le).

## 1. Récupérer le code

```powershell
git clone https://github.com/thias110/gta6-streamdls5.git
cd gta6-streamdls5
git checkout feat/robustesse-qualite   # corrections + améliorations (sans le SDK NVIDIA)
```

La branche `feat/nvidia-vfx` (backend NVIDIA, étape 6) n'est testée qu'après les étapes 2 à 5.

## 2. Compiler (build par défaut, sans SDK NVIDIA)

```powershell
.\build.ps1
```

Résultat attendu : `build\Release\rpenhance.exe`. ImGui est téléchargé automatiquement.

- **Si le build échoue :** copie les 30 dernières lignes d'erreur (la première erreur compte le plus)
  et envoie-les. Les erreurs possibles les plus probables : en-tête manquant, type D3D11/WinRT, option
  `/W4` en avertissement.

## 3. Tests de logique (sans GPU)

```powershell
cd build
ctest -C Release --output-on-failure
cd ..
```

Résultat attendu : 2 tests réussis (`dedupe`, `scene_cut`). Ces tests couvrent le dédoublonnage et la
détection de coupure de scène, mais pas le rendu.

## 4. Test d'auto-vérification et mire synthétique (GPU, sans PS5)

```powershell
.\build\Release\rpenhance.exe --selftest
Get-Content .\build\Release\rpenhance.log
```

À regarder dans le journal : les PSNR de l'interpolation (comparés à répéter la frame ou au fondu),
le coût GPU par frame unique, l'absence de faux positif de coupure, et les PSNR avec/sans protection
du HUD et avec `--flow-scale 0.5`. Les chiffres du README datent d'avant ces branches : c'est ici
qu'on les remesure. Les images de comparaison sont écrites dans `build\Release\selftest\`.

Puis en visuel :

```powershell
.\build\Release\rpenhance.exe --test-pattern --windowed
```

- Raccourcis globaux : **Ctrl+Alt+S** stats, **U** upscale, **F** frame generation, **C**
  comparaison avant/après, **V** visualisation du flow, **Q** quitter.
- À vérifier : pas de crash, pas d'écran noir, mouvement fluide, HUD de la mire stable, stats
  cohérentes. Redimensionne la fenêtre plusieurs fois : pas de fuite ni de plantage.

## 5. Test réel avec la PS5

1. Lance PS Remote Play (ou chiaki), fenêtre en plein écran, 1080p / 60 fps.
2. Lance `.\build\Release\rpenhance.exe` (option utile : `--list` pour voir les fenêtres).
3. Active **NVIDIA App > Système > Vidéo > Super Resolution** (qualité 3 ou 4), sinon le VSR fait un
   simple scaling.
4. Compare avec **Ctrl+Alt+C** (avant/après).

Ce qu'il faut regarder, et noter :
- **Manette :** elle doit rester sur la Remote Play (fais Alt+Tab vers elle si besoin).
- **Fluidité :** image figée par moments ? Si oui, essaie `--delay 1.3`.
- **Doublons :** si des frames sont répétées à tort, essaie `--dup-threshold` plus haut ou
  `--no-adaptive-dedupe`.
- **Coupures :** une caméra très rapide qui désactive l'interpolation à tort ? Essaie
  `--simple-cuts`.
- **HUD :** textes qui bavent ou tremblent ? Compare avec `--no-hud-protect`.
- **Performance :** teste `--flow-scale 0.75` et `0.5` si le GPU est chargé.
- **Latence :** sans caméra haute vitesse, au moins un test « à la main » : `--no-fg` contre FG
  active, en visant.
- Le journal est dans `build\Release\rpenhance.log`.

## 6. Backend NVIDIA (branche `feat/nvidia-vfx`)

> **Avant de commencer :** si tu as collé une clé dans un chat, **révoque-la** (ngc.nvidia.com, Setup,
> API Keys) et génères-en une nouvelle. Ne la partage jamais.

Exigences : GPU **Ada ou Blackwell** (RTX 40 / 50) pour la génération d'images (VFG), pilote **570.65
ou plus**. Le VSR marche aussi sur RTX 20/30.

1. **Compte NGC :** sur https://catalog.ngc.nvidia.com, accepte les conditions d'accès au « Video
   Effects SDK », puis crée une clé personnelle (Setup, Generate Personal Key).
2. **Lis la licence** affichée par NVIDIA. Ne publie **aucun benchmark** du backend NVIDIA avant d'avoir
   vérifié ce qu'elle dit à ce sujet.
3. **Passe à la branche et installe le SDK.** La clé n'est jamais passée en argument ni écrite dans un
   fichier :

   ```powershell
   git checkout feat/nvidia-vfx
   # PowerShell 7 :
   $env:NGC_API_KEY = Read-Host -MaskInput 'Clé NGC'
   # PowerShell 5.1 :
   # $s = Read-Host -AsSecureString 'Clé NGC'; $env:NGC_API_KEY = [Net.NetworkCredential]::new('', $s).Password

   .\scripts\install-nvvfx.ps1 -Gpu 120      # 120 = Blackwell (RTX 50), 89 = Ada (RTX 40)
   Remove-Item Env:NGC_API_KEY
   ```

   Le SDK s'installe dans `third_party\nvvfx-sdk` (ignoré par git, jamais à publier).
   **Ce script n'a jamais tourné** : l'API NGC, la structure de l'archive et le nom de la feature VFG
   sont non vérifiés. S'il échoue, copie le message d'erreur (sans clé) : on corrige. Sinon,
   installe le SDK à la main depuis NGC et passe `-DNVVFX_SDK_DIR=<dossier>` à CMake.

4. **Compile avec l'option :**

   ```powershell
   cmake -S . -B build -A x64 -DRPENHANCE_NVVFX=ON
   cmake --build build --config Release
   ```

   Points à noter si ça échoue : sélecteurs d'effet absents des en-têtes (`#error`), `NvCV_API`,
   options `/W4 /permissive-`.

5. **Teste dans cet ordre :**

   ```powershell
   .\build\Release\rpenhance.exe --test-pattern --windowed --upscale nvvfx
   .\build\Release\rpenhance.exe --test-pattern --windowed --interp nvvfx
   .\build\Release\rpenhance.exe --selftest
   .\build\Release\rpenhance.exe --interp nvvfx --upscale nvvfx     # avec la Remote Play
   ```

   - Le premier chargement d'un modèle peut figer l'image plusieurs secondes : normal.
   - Si le SDK est absent ou le GPU non supporté, rpenhance doit **retomber** sur le flow maison et le
     VSR classique et l'écrire dans le journal. Vérifie ce repli (renomme temporairement le dossier du
     SDK).
   - En 1440p à 144 Hz, le budget peut ne pas tenir : essaie `--vfg-mode low`.
   - Compare qualité et latence avec le flow maison (`--interp flow`) sur de vraies séquences.

6. **Ne publie jamais** le dossier `third_party\`, des DLL ou des modèles NVIDIA, ni une clé.

## 7. Comparer avec les autres outils

Pour savoir si rpenhance fait mieux (Veyra, SmoovPlay, Lossless Scaling) : même jeu, même scène
(rotation rapide de la caméra, végétation, HUD), même réglage Remote Play (1080p, 60 fps), mêmes
conditions réseau (Ethernet de préférence). Compare :
- la **netteté** de l'image fixe ;
- les **artefacts** autour des bords en mouvement et du HUD ;
- la **latence** : au minimum à la main, idéalement avec une caméra qui filme manette et écran ;
- la **stabilité** sur 30 minutes (pas de plantage ni de fuite mémoire GPU).

## 8. Quoi me renvoyer

Pour chaque étape : réussie ou non, la première erreur complète si échec, le contenu utile de
`rpenhance.log` (jamais de clé), et ton ressenti en jeu (fluidité, latence, artefacts). Avec ça, on
corrige point par point.
