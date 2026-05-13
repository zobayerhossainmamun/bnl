$ErrorActionPreference = "Stop"

$Version      = if ($env:VERSION) { $env:VERSION } else { "v2.0.0" }
$RepoRoot     = (Resolve-Path "$PSScriptRoot\..").Path
$ResourcesDir = Join-Path $RepoRoot "resources"
$InstallerDir = Join-Path $RepoRoot "installer\windows"
$OutDir       = Join-Path $RepoRoot "binaries"
$LicenseFile  = Join-Path $ResourcesDir "LICENSE.txt"
$ReadmeFile   = Join-Path $ResourcesDir "README.txt"

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

foreach ($f in @($LicenseFile, $ReadmeFile)) {
    if (-not (Test-Path $f)) {
        Write-Error "Missing required file: $f"
        exit 1
    }
}

$Targets = @(
    @{ Name = "windows-x64"; BuildDir = "build\windows-release" },
    @{ Name = "windows-x86"; BuildDir = "build\windows-x86-release" }
)

foreach ($t in $Targets) {
    $bnlExe = Join-Path $RepoRoot (Join-Path $t.BuildDir "bin\bnl.exe")
    if (-not (Test-Path $bnlExe)) {
        Write-Host "skip   $($t.Name): $bnlExe not built" -ForegroundColor Yellow
        continue
    }

    $stem  = "bnlang-$($t.Name)-$Version"
    $stage = Join-Path ([IO.Path]::GetTempPath()) ("bnl-pkg-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    Copy-Item -Force $bnlExe      (Join-Path $stage "bnl.exe")
    Copy-Item -Force $LicenseFile (Join-Path $stage "LICENSE.txt")
    Copy-Item -Force $ReadmeFile  (Join-Path $stage "README.txt")

    $zipOut = Join-Path $OutDir "$stem.zip"
    if (Test-Path $zipOut) { Remove-Item -Force $zipOut }
    # Zip the staged files directly (no wrapper folder inside the .zip).
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zipOut

    Remove-Item -Recurse -Force $stage
    Write-Host "build  $zipOut"
}

# --- Installers ---------------------------------------------------------
$installerNames = @(
    "bnlang-windows-x64-$Version-installer.exe",
    "bnlang-windows-x86-$Version-installer.exe"
)
foreach ($n in $installerNames) {
    $src = Join-Path $InstallerDir $n
    if (Test-Path $src) {
        Copy-Item -Force $src (Join-Path $OutDir $n)
        Write-Host "copy   $(Join-Path $OutDir $n)"
    } else {
        Write-Host "skip   installer $n not present" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "Done. Artifacts in: $OutDir"
