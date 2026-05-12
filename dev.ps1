# Dot-source this from a regular PowerShell to set up the bnl dev environment:
#   PS> . .\dev.ps1            # x64 (default)
#   PS> . .\dev.ps1 -Arch x86   # x86 — required for the windows-x86 / -release preset
#
# Reloads PATH and VCPKG_ROOT from the registry (so newly-installed tools like
# cmake / vcpkg / ninja are visible) and loads the MSVC + Windows SDK env so
# that cl.exe and link.exe can find kernel32.lib etc. Safe to re-run for the
# same arch. To switch arch, open a fresh PowerShell window (the VS dev shell
# can't be cleanly re-entered with a different target).

param(
    [ValidateSet('x64', 'x86')]
    [string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'

# 1. Refresh PATH from registry (Machine + User scopes).
$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [System.Environment]::GetEnvironmentVariable('Path', 'User')

# 2. Resolve the desired VCPKG_ROOT (User env > default). Applied AFTER the
#    dev shell loads, since Enter-VsDevShell overrides VCPKG_ROOT to point at
#    VS's own bundled vcpkg (which requires a builtin-baseline we don't set).
$desiredVcpkg = [System.Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User')
if (-not $desiredVcpkg) { $desiredVcpkg = 'C:\vcpkg' }

# 3. Skip dev shell setup if already inside one — but verify the target arch
#    matches. The dev shell can't be cleanly re-entered with a different
#    target, so a mismatch means "open a fresh PowerShell window."
if ($env:VSCMD_VER) {
    if ($env:VSCMD_ARG_TGT_ARCH -and $env:VSCMD_ARG_TGT_ARCH -ne $Arch) {
        throw "VS Dev Shell already active for arch '$env:VSCMD_ARG_TGT_ARCH', but '-Arch $Arch' was requested. Open a fresh PowerShell window and re-run."
    }
    $env:VCPKG_ROOT = $desiredVcpkg
    Write-Host "VS Dev Shell already active (VS $($env:VSCMD_VER), $Arch); VCPKG_ROOT=$env:VCPKG_ROOT" -ForegroundColor Green
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
    -DevCmdArguments "-arch=$Arch -no_logo" | Out-Null

# 6. Override VCPKG_ROOT (Dev Shell just set it to VS's bundled vcpkg).
$env:VCPKG_ROOT = $desiredVcpkg

Write-Host "VS Dev Shell loaded (VS $($env:VSCMD_VER), $Arch); VCPKG_ROOT=$env:VCPKG_ROOT" -ForegroundColor Green
