# Guide : compiler et tester rpenhance sous Windows

> **État honnête.** Le code des branches `fix/timing-grid`, `feat/robustesse-qualite`,
> `feat/nvidia-vfx` et `perf/flow` a été écrit et relu sans compilateur MSVC ni GPU : seule la syntaxe et la logique CPU
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

Résultat attendu : tous les tests réussis (`dedupe`, `scene_cut`, `tap_protocol`, `capture_logic`,
`audio_delay`). Ils couvrent le dédoublonnage, la détection de coupure de scène, le protocole du frame
tap, la logique de la carte de capture (horodatage, choix du mode/format/périphérique, doublons sur un
signal câble) et le retard du son relayé, mais pas le rendu ni une vraie carte.

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

## 5 bis. Carte de capture HDMI (branche `feat/hdmi-merge`)

But : vérifier que rpenhance marche aussi bien avec la PS5 branchée sur une carte de capture qu'avec la
Remote Play, **sans régression** de la Remote Play. Rien de ceci n'a été testé sur une vraie carte.

**0. Sans carte (à faire en premier)**

```powershell
.\build\Release\rpenhance.exe --source capture --test-pattern
```

Fenêtre plein écran qui prend le focus (Échap quitte), mire « profil câble ». Dans l'overlay :
`capturé ~60`, `unique ~30`, seuil de doublon autour de 0.4, rythme 2 verrouillé, aucune coupure de
plan. Puis refais l'étape 5 (Remote Play, `--source wgc` par défaut) pour vérifier que rien n'a changé :
overlay passif, la manette reste sur la Remote Play, seuil de base 1.2.

**1. Lister les appareils**

PS5 : **Paramètres > Système > HDMI > Activer HDCP : désactivé**. Branche la carte (USB 3), puis :

```powershell
.\build\Release\rpenhance.exe --list-devices
.\build\Release\rpenhance.exe --list-devices --source capture        # + modes natifs de la carte retenue
.\build\Release\rpenhance.exe --list-devices --device "Elgato"       # + modes natifs de cette carte
```

À noter : le nom exact de la carte vidéo, celui de son entrée audio, et la liste des modes (taille,
fréquence, format NV12/YUY2/MJPEG...).

**2. Lancer**

```powershell
.\build\Release\rpenhance.exe --source capture                       # ou --hdmi
.\build\Release\rpenhance.exe --source capture --mode 1920x1080@60   # pour forcer un mode
Get-Content .\build\Release\rpenhance.log
```

Dans le journal, relève : `mode choisi …, sortie NV12|RGB32`, `images livrées sur le GPU (zéro copie)` ou
`en mémoire système (1 copie)`, `horodatage = …` (horloge QPC du pilote, horodatage MF recalé, ou
arrivée), `conversion NV12 … (BT.601/709, plage …)`, et la ligne `Son : "…" -> sortie par défaut`.
En cas d'échec, copie le message complet (boîte de dialogue et journal).

**3. Vérifier l'image**

- Couleurs : comparer avec la TV branchée sur la recopie HDMI (ni délavées ni écrasées : sinon noter la
  ligne `conversion NV12 …` et le réglage « Gamme RVB » de la PS5).
- Pas de bandes noires inutiles, pas de recadrage (l'image doit être complète, HUD compris).
- Jeu à 30 fps : overlay `capturé ~60`, `unique ~30`, rythme 2 ; image fluide avec FG.
- Débrancher l'USB en cours de route : rpenhance doit s'arrêter proprement (« La carte de capture a
  cessé d'envoyer des images » dans le journal), sans plantage.

**4. Vérifier le son**

- Le son sort par la sortie audio par défaut de Windows ; volume dans l'overlay.
- Synchronisation : avec FG active, l'overlay affiche `Retard du son : N ms (auto)` ; N doit suivre
  « retard FG » − « arrivée » et tomber à 0 après Ctrl+Alt+F (FG coupée). Juge à l'oreille sur une scène
  avec des bruits secs (tir, porte) : si le son est en avance ou en retard, essaie `--audio-delay 20`,
  `40`, `60`… et note la valeur qui va le mieux. `--audio-delay off` reproduit l'ancien comportement.
- Si aucune entrée audio n'est trouvée : `--audio-device "nom"` (nom donné par `--list-devices`).

**5. Comparer la latence avec la Remote Play**

Même jeu, même scène, même écran. Pour chaque mode (`--source wgc` avec la Remote Play, puis
`--source capture`), et chaque fois avec et sans `--no-fg` :
- noter les valeurs « arrivée » et « retard FG » de l'overlay (moyennes sur ~30 s, ou les lignes du
  journal toutes les 5 s) ;
- au mieux, filmer manette et écran avec un téléphone en ralenti (240 fps) et compter les images entre
  l'appui et la réaction à l'écran, sur une dizaine d'appuis.

Aucun chiffre n'est donné ici à l'avance : ce sont ces mesures qui diront l'écart réel entre les deux
modes.

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

## 7. Comparer les temps GPU du flow : `feat/nvidia-vfx` contre `perf/flow`

La branche `perf/flow` (locale tant qu'elle n'est pas publiée) optimise l'estimation de mouvement
**sans changer son résultat attendu** : SAD des blocs en `GatherRed` (32 lectures de texture par
vecteur candidat au lieu de 128) et flow aller + retour dans les mêmes Dispatch (17 au lieu de 34 par
frame unique avec 5 niveaux de pyramide). Ces chiffres sont des **comptes faits en lisant le code**,
pas des mesures : seul ce test dira si le GPU va plus vite. Comme le reste, cette branche n'a été ni
compilée avec MSVC ni lancée sur GPU (syntaxe HLSL vérifiée avec dxc en profil 6_0 ; l'appli compile
en 5_0 avec FXC, à vérifier ici).

1. Compile et lance le selftest **5 fois** sur chaque branche, même build (Release), PC au repos
   (navigateur et jeux fermés, même mode d'alimentation) :

   ```powershell
   git checkout feat/nvidia-vfx
   .\build.ps1
   1..5 | % { .\build\Release\rpenhance.exe --selftest | Out-Null; Select-String 'GPU : pyramide|PSNR frame du milieu|flow-scale 0.5|SELFTEST' .\build\Release\rpenhance.log }

   git checkout perf/flow
   .\build.ps1
   1..5 | % { .\build\Release\rpenhance.exe --selftest | Out-Null; Select-String 'GPU : pyramide|PSNR frame du milieu|flow-scale 0.5|SELFTEST' .\build\Release\rpenhance.log }
   ```

2. Note pour chaque lancement la valeur **« flow aller+retour … ms »** de la ligne
   `GPU : pyramide …` (moyenne lissée des 60 itérations de fin de selftest). `| Out-Null` fait
   attendre la fin de l'exe ; le journal est réécrit à chaque lancement. Compare les **médianes** des 5 lancements :
   un écart de moins de ~5 % est du bruit.
3. **La qualité ne doit pas bouger** : les PSNR (« PSNR frame du milieu », « flow-scale 0.5 »,
   « protection du HUD ») doivent être **identiques** entre les deux branches (même calcul, même ordre
   des sommes), et `SELFTEST OK` sur les deux. Un écart de plus de 0,05 dB veut dire que
   l'optimisation a changé le calcul : envoie les deux journaux, c'est un bug.
4. Compare aussi `selftest\flow.bmp` des deux branches (doit être identique à l'œil).
5. En usage réel (étape 5), l'overlay affiche `flow x.xx` (ms GPU) : compare sur la même scène.

Points à surveiller spécifiquement sur `perf/flow` : une erreur FXC à la compilation des shaders
`cs_motion_search`, `cs_motion_propagate` ou `cs_flow_median` au démarrage (copie le message du
journal) ; avec `--debug` (couche de debug D3D11, si installée), un avertissement sur des ressources
liées à la fois en lecture et en écriture.

## 8. Comparer avec les autres outils

Pour savoir si rpenhance fait mieux (Veyra, SmoovPlay, Lossless Scaling) : même jeu, même scène
(rotation rapide de la caméra, végétation, HUD), même réglage Remote Play (1080p, 60 fps), mêmes
conditions réseau (Ethernet de préférence). Compare :
- la **netteté** de l'image fixe ;
- les **artefacts** autour des bords en mouvement et du HUD ;
- la **latence** : au minimum à la main, idéalement avec une caméra qui filme manette et écran ;
- la **stabilité** sur 30 minutes (pas de plantage ni de fuite mémoire GPU).

## 9. Quoi me renvoyer

Pour chaque étape : réussie ou non, la première erreur complète si échec, le contenu utile de
`rpenhance.log` (jamais de clé), et ton ressenti en jeu (fluidité, latence, artefacts). Avec ça, on
corrige point par point.
