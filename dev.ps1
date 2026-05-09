# Dot-source this from a regular PowerShell to set up the bnl dev environment:
#   PS> . .\dev.ps1
#
# Reloads PATH and VCPKG_ROOT from the registry (so newly-installed tools like
# cmake / vcpkg / ninja are visible) and loads the MSVC + Windows SDK env so
# that cl.exe and link.exe can find kernel32.lib etc. Safe to re-run.

$ErrorActionPreference = 'Stop'

# 1. Refresh PATH from registry (Machine + User scopes).
$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [System.Environment]::GetEnvironmentVariable('Path', 'User')

# 2. Resolve the desired VCPKG_ROOT (User env > default). Applied AFTER the
#    dev shell loads, since Enter-VsDevShell overrides VCPKG_ROOT to point at
#    VS's own bundled vcpkg (which requires a builtin-baseline we don't set).
$desiredVcpkg = [System.Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
if (-not $desiredVcpkg) { $desiredVcpkg = 'C:\vcpkg' }

# 3. Skip dev shell setup if already inside one, but still pin VCPKG_ROOT.
if ($env:VSCMD_VER) {
    $env:VCPKG_ROOT = $desiredVcpkg
    Write-Host "VS Dev Shell already active (VS $($env:VSCMD_VER)); VCPKG_ROOT=$env:VCPKG_ROOT" -ForegroundColor Green
    return
}

# 4. Locate Visual Studio with C++ tools via vswhere.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at $vswhere. Install Visual Studio with the C++ workload."
}

$installPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $installPath) {
    throw "No Visual Studio install with the C++ Tools component was found."
}

# 5. Enter the VS Dev Shell into the current PowerShell process.
$devShell = Join-Path $installPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell
Enter-VsDevShell -VsInstallPath $installPath `
    -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -no_logo' | Out-Null

# 6. Override VCPKG_ROOT (Dev Shell just set it to VS's bundled vcpkg).
$env:VCPKG_ROOT = $desiredVcpkg

Write-Host "VS Dev Shell loaded (VS $($env:VSCMD_VER)); VCPKG_ROOT=$env:VCPKG_ROOT" -ForegroundColor Green
