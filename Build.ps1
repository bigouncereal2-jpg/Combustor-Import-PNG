param([Parameter(Mandatory = $true)][string]$GameDirectory)

$ErrorActionPreference = 'Stop'
$GameDirectory = (Resolve-Path -LiteralPath $GameDirectory).Path
if (-not (Test-Path -LiteralPath (Join-Path $GameDirectory 'MelonLoader\Il2CppAssemblies\Assembly-CSharp.dll'))) {
    throw 'Launch Combustor with MelonLoader once to generate its game bindings.'
}
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'Run this script from an x64 Native Tools Command Prompt for Visual Studio.'
}

$dist = Join-Path $PSScriptRoot 'dist'
$bundle = Join-Path $dist 'package'
$nativeBuild = Join-Path $dist 'native'
$mods = Join-Path $bundle 'Mods'
$decoder = Join-Path $bundle 'UserData\CombustorPng'
New-Item -ItemType Directory -Force -Path $mods, $decoder, $nativeBuild | Out-Null

$project = Join-Path $PSScriptRoot 'src\managed\PngMod.csproj'
$managedBuild = Join-Path $dist 'managed'
& dotnet build $project -c Release --output $managedBuild "-p:GameDirectory=$GameDirectory"
if ($LASTEXITCODE -ne 0) { throw 'The C# mod failed to build.' }
$mod = Join-Path $managedBuild 'CombustorPngImport.dll'
Copy-Item -LiteralPath $mod -Destination $mods -Force

$wrapper = Join-Path $PSScriptRoot 'src\native\Decoder.cpp'
$pngSource = Join-Path $PSScriptRoot 'src\native\png_image.cpp'
Push-Location -LiteralPath $nativeBuild
try {
    & cl.exe /nologo /std:c++20 /O2 /EHsc /MT /LD $wrapper $pngSource /link /MACHINE:X64 /OUT:CombustorPngDecoder.dll ole32.lib windowscodecs.lib user32.lib comdlg32.lib gdi32.lib shell32.lib
    if ($LASTEXITCODE -ne 0) { throw 'The PNG decoder failed to build.' }
    Copy-Item -LiteralPath (Join-Path $nativeBuild 'CombustorPngDecoder.dll') -Destination $decoder -Force
} finally {
    Pop-Location
}

Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'INSTALL.txt'), (Join-Path $PSScriptRoot 'LICENSE') -Destination $bundle -Force
$archive = Join-Path $dist 'CombustorPngMod.zip'
Compress-Archive -LiteralPath $mods, (Join-Path $bundle 'UserData'), (Join-Path $bundle 'INSTALL.txt'), (Join-Path $bundle 'LICENSE') -DestinationPath $archive -Force
Write-Output $archive
