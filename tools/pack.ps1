param(
    [string]$Preset = 'msvc-release',
    [string]$GameScrcpy = $env:GS_HOME,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$repository = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content (Join-Path $repository 'extension\manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$package = Join-Path $repository "build\$Preset\package"
$dist = Join-Path $repository 'dist'
$output = Join-Path $dist ('{0}-{1}.gsext' -f $manifest.id, $manifest.version)

if (-not $GameScrcpy) {
    throw 'Set GS_HOME to the folder of GameScrcpy that holds gs.exe, or pass -GameScrcpy <folder>.'
}
$gs = Join-Path $GameScrcpy 'gs.exe'
if (-not (Test-Path $gs)) {
    throw "gs.exe is not in $GameScrcpy. GS_HOME has to name the folder of GameScrcpy 2.1 or later."
}

if (-not $SkipBuild) {
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'Run this from a Developer PowerShell for Visual Studio 2022 (x64), so that cl.exe and Ninja are found.'
    }
    Push-Location $repository
    try {
        & cmake --preset $Preset
        if ($LASTEXITCODE -ne 0) {
            throw "Configuring with the preset $Preset failed"
        }
        & cmake --build --preset $Preset --target gs_ext_esp32
        if ($LASTEXITCODE -ne 0) {
            throw 'Building esp32.dll failed'
        }
    } finally {
        Pop-Location
    }
}

$expected = @(
    'manifest.json',
    'esp32.dll',
    'scripts\panel.ts',
    'icons\board.svg',
    'help\esp32.md',
    'help\esp32.zh_CN.md',
    'firmware\firmware.json',
    'LICENSE.txt'
)
foreach ($file in $expected) {
    if (-not (Test-Path (Join-Path $package $file))) {
        throw "The package folder $package has no $file; build it with the preset $Preset first."
    }
}
$firmware = Get-Content (Join-Path $package 'firmware\firmware.json') -Raw | ConvertFrom-Json
foreach ($target in $firmware.targets.PSObject.Properties) {
    $image = Join-Path $package ('firmware\' + $target.Value.file)
    if (-not (Test-Path $image)) {
        throw "The firmware image $($target.Value.file) for $($target.Name) is missing from the package."
    }
    $hash = (Get-FileHash -Algorithm SHA256 $image).Hash.ToLowerInvariant()
    if ($hash -ne $target.Value.sha256) {
        throw "The firmware image $($target.Value.file) does not match the SHA-256 in firmware.json."
    }
}
$strays = Get-ChildItem -Recurse -File $package | Where-Object { $_.Extension -in @('.pdb', '.ilk', '.exp', '.lib') }
if ($strays) {
    throw "The package folder holds build files that do not belong in the package: $($strays.Name -join ', ')"
}

New-Item -ItemType Directory -Force -Path $dist | Out-Null
if (Test-Path $output) {
    Remove-Item $output
}
& $gs pack $package -o $output
if ($LASTEXITCODE -ne 0) {
    throw "gs pack refused the package (exit code $LASTEXITCODE)"
}
Write-Host "Wrote $output"
