<#
.SYNOPSIS
    Standalone installer for the Bantu toolchain on Windows.

.DESCRIPTION
    Detects your architecture, downloads the matching release binary, installs it
    to %LOCALAPPDATA%\Bantu\bin, adds that directory to your user PATH, and verifies
    `bantu --version`. No administrator rights required.

.EXAMPLE
    irm https://raw.githubusercontent.com/AsseySilivestir/Bantu/main/scripts/installers/install.ps1 | iex

.PARAMETER Version
    Release tag to install (default: latest).

.PARAMETER LocalBin
    Path to an already-built bantu.exe to install instead of downloading
    (offline / CI / testing).
#>
[CmdletBinding()]
param(
    [string]$Version = $env:BANTU_VERSION,
    [string]$LocalBin = $env:BANTU_LOCAL_BIN,
    [string]$Repo = $(if ($env:BANTU_REPO) { $env:BANTU_REPO } else { "AsseySilivestir/Bantu" })
)

$ErrorActionPreference = "Stop"

function Info($m) { Write-Host "  $m" }
function Ok($m)   { Write-Host "  [ok] $m" -ForegroundColor Green }
function Die($m)  { Write-Host "  error: $m" -ForegroundColor Red; exit 1 }

Write-Host ""
Write-Host "  Bantu toolchain installer" -ForegroundColor White
Write-Host "  $Repo"
Write-Host ""

# ── Detect architecture ────────────────────────────────────────────────────
$archRaw = $env:PROCESSOR_ARCHITECTURE
switch ($archRaw) {
    "AMD64" { $Arch = "x64" }
    "ARM64" { $Arch = "arm64" }
    "x86"   { $Arch = "x86" }
    default { Die "unsupported architecture: $archRaw" }
}
Info "platform: windows-$Arch"

# ── Install location ────────────────────────────────────────────────────────
$InstallDir = Join-Path $env:LOCALAPPDATA "Bantu\bin"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$Target = Join-Path $InstallDir "bantu.exe"
Info "install dir: $InstallDir"

# ── Acquire the binary ──────────────────────────────────────────────────────
if ($LocalBin) {
    if (-not (Test-Path $LocalBin)) { Die "LocalBin not found: $LocalBin" }
    Info "installing local binary: $LocalBin"
    Copy-Item -Force $LocalBin $Target
}
else {
    if (-not $Version) { $Version = "latest" }
    if ($Version -eq "latest") {
        Info "resolving latest release..."
        $rel = Invoke-RestMethod -UseBasicParsing "https://api.github.com/repos/$Repo/releases/latest"
        $Version = $rel.tag_name
        if (-not $Version) { Die "could not resolve latest version (pass -Version)" }
    }
    Info "version: $Version"

    $Asset = "bantu-windows-$Arch.zip"
    $Url   = "https://github.com/$Repo/releases/download/$Version/$Asset"
    $Tmp   = Join-Path ([System.IO.Path]::GetTempPath()) ("bantu-" + [guid]::NewGuid().ToString())
    New-Item -ItemType Directory -Force -Path $Tmp | Out-Null
    try {
        $Zip = Join-Path $Tmp $Asset
        Info "downloading $Asset..."
        Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile $Zip
        Info "extracting..."
        Expand-Archive -Force -Path $Zip -DestinationPath $Tmp
        $bin = Get-ChildItem -Path $Tmp -Recurse -Filter "bantu.exe" | Select-Object -First 1
        if (-not $bin) { Die "archive did not contain bantu.exe" }
        Copy-Item -Force $bin.FullName $Target
    }
    finally {
        Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
    }
}
Ok "installed $Target"

# ── Add to user PATH ────────────────────────────────────────────────────────
$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if (-not $userPath) { $userPath = "" }
if (($userPath -split ";") -notcontains $InstallDir) {
    $newPath = if ($userPath.TrimEnd(";")) { "$($userPath.TrimEnd(';'));$InstallDir" } else { $InstallDir }
    [Environment]::SetEnvironmentVariable("Path", $newPath, "User")
    $env:Path = "$env:Path;$InstallDir"
    Ok "added $InstallDir to your user PATH (restart terminals to pick it up)"
}

# ── Verify ──────────────────────────────────────────────────────────────────
Write-Host ""
try {
    $v = & $Target --version 2>$null
    Ok "$v ready"
    Write-Host ""
    Write-Host "  Get started:  bantu run yourfile.b" -ForegroundColor White
    Write-Host ""
}
catch {
    Die "installed but '$Target --version' failed to run"
}
