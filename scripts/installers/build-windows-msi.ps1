<#
.SYNOPSIS
    Build a Windows MSI for the Bantu toolchain from scripts/installers/bantu.wxs.

.DESCRIPTION
    Wraps the WiX .NET tool. Installs WiX if it is not already present, stages the
    provided bantu.exe, and produces dist\Bantu-<Version>-<Arch>.msi.

.EXAMPLE
    scripts\installers\build-windows-msi.ps1 -Binary .\build\bantu.exe -Version 1.2.2

.PARAMETER Binary
    Path to the built bantu.exe to package.

.PARAMETER Version
    Product version (default 1.2.2).

.PARAMETER Arch
    Architecture label for the output file name (default x64).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Binary,
    [string]$Version = "1.2.2",
    [string]$Arch = "x64"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$wxs  = Join-Path $PSScriptRoot "bantu.wxs"

if (-not (Test-Path $Binary)) { throw "binary not found: $Binary" }

# Ensure the WiX tool is available.
if (-not (Get-Command wix -ErrorAction SilentlyContinue)) {
    Write-Host "Installing WiX .NET tool..."
    dotnet tool install --global wix
    $env:PATH = "$env:PATH;$env:USERPROFILE\.dotnet\tools"
}

# Stage bantu.exe in a bindpath directory (the .wxs references it by name).
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("bantu-msi-" + [guid]::NewGuid().ToString())
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item -Force $Binary (Join-Path $stage "bantu.exe")

$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$out = Join-Path $dist "Bantu-$Version-$Arch.msi"

Write-Host "Building $out ..."
wix build $wxs -d "Version=$Version" -bindpath $stage -arch $Arch -o $out

Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
Write-Host "Built: $out"
