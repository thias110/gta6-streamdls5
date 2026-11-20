<#
.SYNOPSIS
    Télécharge le NVIDIA Video Effects (VFX) SDK Core depuis NVIDIA NGC et installe les features
    Video Super Resolution et Video Frame Generation, pour le backend optionnel --interp/--upscale nvvfx.

.DESCRIPTION
    - La clé API NGC est lue UNIQUEMENT dans la variable d'environnement NGC_API_KEY. Elle n'est jamais
      acceptée en argument, jamais écrite sur disque, jamais affichée ni journalisée. Elle est transmise
      aux outils NVIDIA (NGC CLI, install_feature.ps1) par la variable d'environnement NGC_CLI_API_KEY
      du processus courant, restaurée à la fin.
    - Les fichiers téléchargés (DLL, en-têtes, modèles) sont sous licence NVIDIA (Maxine / VFX SDK) :
      ils restent dans le dossier de destination, ignoré par git (third_party/), et ne doivent pas être
      redistribués avec rpenhance.
    - Il faut un compte NGC, avoir accepté les conditions d'accès au VFX SDK sur catalog.ngc.nvidia.com
      et une clé personnelle (NGC > Setup > Generate Personal Key).

    Exemple :
        $env:NGC_API_KEY = Read-Host -MaskInput 'Clé NGC'   # PowerShell 7 (voir le message d'erreur pour 5.1)
        .\scripts\install-nvvfx.ps1 -Gpu 120                 # 120 = Blackwell, 89 = Ada
        Remove-Item Env:NGC_API_KEY
        cmake -S . -B build -A x64 -DRPENHANCE_NVVFX=ON

    ÉTAT : écrit sans accès à Windows ni à un compte NGC. Les points marqués « à valider » (URL de
    l'API NGC, structure de l'archive, nom exact de la feature VFG) n'ont pas pu être vérifiés.

.PARAMETER Version
    Version de la ressource NGC nvidia/maxine/vfx_sdk_core (ex. 1.3.0.0_windows). Défaut : la plus
    récente se terminant par _windows (interrogation de l'API NGC).

.PARAMETER Gpu
    Compute capability passée à install_feature.ps1 (-gpu) : 89 (Ada), 120 (Blackwell), 75/86 (VSR seul,
    VFG non supporté). Défaut : détection automatique par le script NVIDIA.

.PARAMETER Destination
    Dossier d'installation. Défaut : <dépôt>\third_party\nvvfx-sdk (ignoré par git, et dossier par défaut
    de NVVFX_SDK_DIR dans CMakeLists.txt).

.PARAMETER Features
    Features à installer. Défaut : nvvfxvideosuperres, nvvfxvideoframegeneration.

.PARAMETER SkipFeatures
    N'installe que le SDK Core.
#>
[CmdletBinding()]
param(
    [string]$Version = '',
    [ValidateSet('', '75', '86', '89', '120')][string]$Gpu = '',
    [string]$Destination = '',
    [string[]]$Features = @('nvvfxvideosuperres', 'nvvfxvideoframegeneration'),
    [switch]$SkipFeatures
)
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # Invoke-WebRequest bien plus rapide sans barre de progression

$NgcOrg      = 'nvidia'
$NgcTeam     = 'maxine'
$NgcResource = 'vfx_sdk_core'          # « NVIDIA Video Effects SDK Core » sur catalog.ngc.nvidia.com
$NgcApi      = 'https://api.ngc.nvidia.com/v2'

# --- Clé : variable d'environnement uniquement ------------------------------------------------------
if ([string]::IsNullOrWhiteSpace($env:NGC_API_KEY)) {
    Write-Host ("ERREUR : la variable d'environnement NGC_API_KEY est absente ou vide.`n" +
        "Crée une clé personnelle sur https://ngc.nvidia.com (Setup > Generate Personal Key), puis dans CETTE " +
        "session PowerShell :`n" +
        "    PowerShell 7   : `$env:NGC_API_KEY = Read-Host -MaskInput 'Clé NGC'`n" +
        "    PowerShell 5.1 : `$s = Read-Host -AsSecureString 'Clé NGC'; `$env:NGC_API_KEY = [Net.NetworkCredential]::new('', `$s).Password`n" +
        "La clé n'est jamais acceptée en argument ni lue depuis un fichier.") -ForegroundColor Red
    exit 1
}

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Destination) { $Destination = Join-Path $repo 'third_party\nvvfx-sdk' }
$Destination = [IO.Path]::GetFullPath($Destination)
$download    = Join-Path $Destination '_download'
New-Item -ItemType Directory -Force -Path $Destination, $download | Out-Null

if ($Gpu -in @('75', '86')) {
    Write-Warning "GPU $Gpu : Video Frame Generation n'est supporté que sur Ada (89) et Blackwell (120) ; seul VSR fonctionnera."
}

# La clé ne transite que par l'environnement du processus courant (hérité par les outils NVIDIA lancés
# depuis ce processus), jamais par la ligne de commande.
$previousCliKey = $env:NGC_CLI_API_KEY
$env:NGC_CLI_API_KEY = $env:NGC_API_KEY

function Get-NgcToken {
    # Échange clé -> jeton (flux documenté pour l'API REST NGC ; à valider avec les clés « nvapi- »).
    $pair  = '$oauthtoken:' + $env:NGC_API_KEY
    $basic = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($pair))
    $pair  = $null
    $uri   = "https://authn.nvidia.com/token?service=ngc&scope=group/ngc:$NgcOrg"
    try {
        $r = Invoke-RestMethod -Uri $uri -Headers @{ Authorization = "Basic $basic" } -Method Get -UseBasicParsing
    } catch {
        throw "Authentification NGC refusée (clé invalide, expirée ou sans accès au VFX SDK ?). HTTP : $($_.Exception.Response.StatusCode)"
    } finally {
        $basic = $null
    }
    if (-not $r.token) { throw "Réponse d'authentification NGC inattendue (pas de jeton)." }
    return $r.token
}

function Find-SdkRoot([string]$dir) {
    # Racine = dossier qui contient features\install_feature.ps1 (ou include\nvVideoEffects.h).
    $hit = Get-ChildItem -Path $dir -Recurse -Filter 'install_feature.ps1' -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'features' } | Select-Object -First 1
    if ($hit) { return $hit.Directory.Parent.FullName }
    $hdr = Get-ChildItem -Path $dir -Recurse -Filter 'nvVideoEffects.h' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hdr) { return $hdr.Directory.Parent.FullName }
    return $null
}

try {
    $root = Find-SdkRoot $Destination
    if ($root) {
        Write-Host "SDK Core déjà présent : $root (supprime $Destination pour le réinstaller)"
    } else {
        $ngcCli = Get-Command ngc -ErrorAction SilentlyContinue
        if ($ngcCli) {
            # Voie 1 : NGC CLI officiel (lit NGC_CLI_API_KEY dans l'environnement).
            if (-not $Version) { throw "Avec le NGC CLI, précise -Version (ex. 1.3.0.0_windows) : ngc registry resource list-versions $NgcOrg/$NgcTeam/$NgcResource" }
            Write-Host "Téléchargement via NGC CLI : $NgcOrg/$NgcTeam/${NgcResource}:$Version"
            & $ngcCli.Source registry resource download-version "$NgcOrg/$NgcTeam/${NgcResource}:$Version" --dest $download
            if ($LASTEXITCODE) { throw "NGC CLI a échoué (code $LASTEXITCODE)." }
        } else {
            # Voie 2 : API REST NGC (URL à valider : elles répondent 401 sans authentification, ce qui
            # confirme leur existence mais pas le format exact des réponses).
            $token   = Get-NgcToken
            $headers = @{ Authorization = "Bearer $token" }
            $base    = "$NgcApi/org/$NgcOrg/team/$NgcTeam/resources/$NgcResource"
            if (-not $Version) {
                $v = Invoke-RestMethod -Uri "$base/versions" -Headers $headers -UseBasicParsing
                $ids = @($v.recipeVersions | ForEach-Object { $_.versionId }) + @($v.versions | ForEach-Object { $_.versionId }) |
                    Where-Object { $_ -like '*_windows' } | Sort-Object { [version](($_ -split '_')[0]) } -Descending
                if (-not $ids) { throw "Aucune version *_windows trouvée pour $NgcResource ; précise -Version." }
                $Version = $ids[0]
            }
            Write-Host "Téléchargement de $NgcOrg/$NgcTeam/${NgcResource}:$Version (API REST NGC)"
            $zip = Join-Path $download "$NgcResource-$Version.zip"
            Invoke-WebRequest -Uri "$base/versions/$Version/zip" -Headers $headers -OutFile $zip -UseBasicParsing
            $headers = $null; $token = $null
            Expand-Archive -Path $zip -DestinationPath $download -Force
        }
        # Le paquet Core est livré en .zip (guide VFX SDK) ; NGC peut l'emballer dans une autre archive.
        Get-ChildItem -Path $download -Recurse -Filter '*.zip' | ForEach-Object {
            Expand-Archive -Path $_.FullName -DestinationPath (Join-Path $Destination ([IO.Path]::GetFileNameWithoutExtension($_.Name))) -Force
        }
        $root = Find-SdkRoot $Destination
        if (-not $root) { throw "Archive extraite dans $Destination mais racine du SDK introuvable (features\install_feature.ps1)." }
        Write-Host "SDK Core installé : $root"
    }

    if (-not $SkipFeatures) {
        $installer = Join-Path $root 'features\install_feature.ps1'
        if (-not (Test-Path $installer)) { throw "install_feature.ps1 introuvable sous $root\features." }
        Write-Host "Installation des features : $($Features -join ', ')   (liste complète : $installer -list_features)"
        Push-Location (Split-Path $installer)
        try {
            $global:LASTEXITCODE = 0
            $featureList = $Features -join ','
            if ($Gpu) { & $installer -gpu $Gpu -features $featureList }
            else { & $installer -features $featureList }
            if ($LASTEXITCODE) { throw "install_feature.ps1 a échoué (code $LASTEXITCODE)." }
        } finally { Pop-Location }
    }

    Write-Host ""
    Write-Host "Terminé. SDK : $root"
    Write-Host "Compilation : cmake -S . -B build -A x64 -DRPENHANCE_NVVFX=ON -DNVVFX_SDK_DIR=`"$root`""
    Write-Host "Exécution   : rpenhance.exe --interp nvvfx --upscale nvvfx --nvvfx-dir `"$root`""
    Write-Host "Rappel : ces fichiers sont sous licence NVIDIA ; ne les redistribue pas avec rpenhance."
} finally {
    # Restaure l'environnement du processus : la clé ne reste pas dans NGC_CLI_API_KEY.
    if ($null -eq $previousCliKey) { Remove-Item Env:NGC_CLI_API_KEY -ErrorAction SilentlyContinue }
    else { $env:NGC_CLI_API_KEY = $previousCliKey }
}
