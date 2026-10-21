<#
.SYNOPSIS
    Builds RecoverAside and optionally packages it. Used both locally and by CI.

.EXAMPLE
    .\build.ps1                                   # Release, x64 and x86
    .\build.ps1 -Platform x64 -Package -Version 1.2.0
#>
param(
    [ValidateSet('x64', 'x86')]
    [string[]] $Platform = @('x64', 'x86'),

    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',

    # Zip each platform's exe with the README into .\dist
    [switch] $Package,

    [string] $Version = 'dev'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# Find a Visual Studio installation that has the MFC component.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw 'vswhere.exe not found. Install Visual Studio 2022 or later with the "Desktop development with C++" workload.'
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.ATLMFC -property installationPath
if (-not $vs) {
    throw 'No Visual Studio installation with the "C++ MFC for latest build tools" component was found.'
}
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\MSBuild.exe'
Write-Host "Using $msbuild"

foreach ($p in $Platform) {
    Write-Host "`n=== Building $Configuration|$p ===" -ForegroundColor Cyan
    & $msbuild (Join-Path $root 'RecoverAside.sln') /m /nologo /v:minimal /restore:false `
        "/p:Configuration=$Configuration" "/p:Platform=$p"
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $Configuration|$p (exit code $LASTEXITCODE)." }
}

if ($Package) {
    $dist = Join-Path $root 'dist'
    New-Item -ItemType Directory -Force $dist | Out-Null
    foreach ($p in $Platform) {
        $outPlatform = if ($p -eq 'x86') { 'Win32' } else { 'x64' }
        $exe = Join-Path $root "bin\$outPlatform\$Configuration\RecoverAside.exe"
        if (-not (Test-Path $exe)) { throw "Expected build output not found: $exe" }

        $zip = Join-Path $dist "RecoverAside-$Version-$p.zip"
        if (Test-Path $zip) { Remove-Item $zip -Force }
        Compress-Archive -Path $exe, (Join-Path $root 'README.md') -DestinationPath $zip
        Write-Host "Packaged $zip"
    }
}
