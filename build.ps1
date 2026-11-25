[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$GamePath,

    [string]$ManualPath,

    [string]$CecilPath
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
if (!$ManualPath) { $ManualPath = Join-Path $root 'inputs/SHENZHEN IO Manual (Korean).pdf' }
$game = (Resolve-Path -LiteralPath $GamePath).Path
$manual = (Resolve-Path -LiteralPath $ManualPath).Path
$programFilesX86 = [Environment]::GetFolderPath('ProgramFilesX86')
$vswhere = Join-Path $programFilesX86 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio Build Tools with MSVC x86/x64 is required.' }
$vc = Get-ChildItem -LiteralPath (Join-Path $vs 'VC/Tools/MSVC') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'bin/Hostx64/x86/cl.exe') } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
$kits = Join-Path $programFilesX86 'Windows Kits/10'
$sdk = Get-ChildItem -LiteralPath (Join-Path $kits 'Include') -Directory |
    Where-Object { (Test-Path -LiteralPath (Join-Path $_.FullName 'um/Windows.h')) -and
                   (Test-Path -LiteralPath (Join-Path $kits "bin/$($_.Name)/x64/rc.exe")) -and
                   (Test-Path -LiteralPath (Join-Path $kits "Lib/$($_.Name)/um/x86/bcrypt.lib")) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty Name
if (!$vc -or !$sdk) { throw 'MSVC for x86 and the Windows SDK were not found.' }

if (!$CecilPath) {
    $packs = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'dotnet/packs/Microsoft.Android.Sdk.Windows'
    if (Test-Path -LiteralPath $packs) {
        $CecilPath = Get-ChildItem -LiteralPath $packs -Directory |
            ForEach-Object { Join-Path $_.FullName 'tools/Mono.Cecil.dll' } |
            Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    }
    if (!$CecilPath) {
        $candidate = Join-Path $vs 'Common7/IDE/Extensions/TestPlatform/Extensions/Mono.Cecil.dll'
        if (Test-Path -LiteralPath $candidate) { $CecilPath = $candidate }
    }
}
if (!$CecilPath) { throw 'Specify -CecilPath with a Mono.Cecil DLL.' }
$cecil = (Resolve-Path -LiteralPath $CecilPath).Path
$facade = Join-Path $vs 'MSBuild/Microsoft/Microsoft.NET.Build.Extensions/net461/lib/netstandard.dll'
$csc = Join-Path $vs 'MSBuild/Current/Bin/Roslyn/csc.exe'
if (!(Test-Path -LiteralPath $csc)) { throw 'The Visual Studio Roslyn C# compiler was not found.' }

function Assert-Exit([string]$Step) {
    if ($LASTEXITCODE -ne 0) { throw "$Step failed with exit code $LASTEXITCODE." }
}

$oldPath = $env:Path
$oldInclude = $env:INCLUDE
$oldLib = $env:LIB
Push-Location -LiteralPath $root
try {
    New-Item -ItemType Directory -Force -Path build,release | Out-Null
    $env:Path = "$vc/bin/Hostx64/x86;$kits/bin/$sdk/x64;$oldPath"
    $env:INCLUDE = "$vc/include;$kits/Include/$sdk/ucrt;$kits/Include/$sdk/um;$kits/Include/$sdk/shared;$kits/Include/$sdk/winrt"
    $env:LIB = "$vc/lib/x86;$kits/Lib/$sdk/ucrt/x86;$kits/Lib/$sdk/um/x86"
    & $csc /nologo /target:library /optimize+ /deterministic+ "/pathmap:$root=/_/ShenzhenKoreanPatch" /out:build/KoreanLocaleRuntime.dll src/managed/KoreanLocaleRuntime.cs
    Assert-Exit 'Managed patch helper build'
    [void][Reflection.Assembly]::LoadFrom($cecil)
    $references = @($cecil, 'System.Core.dll')
    if (Test-Path -LiteralPath $facade) { $references += $facade }
    Add-Type -Path (Join-Path $root 'src/managed/Build-KoreanLocale.cs') -ReferencedAssemblies $references
    [BuildKoreanLocale]::Build((Join-Path $game 'Shenzhen.exe'), (Join-Path $root 'build/Shenzhen.patched.exe'),
        (Join-Path $root 'build/KoreanLocaleRuntime.dll'))

    py -3 tools/package.py --game $game --manual $manual
    Assert-Exit 'Payload generation'
    rc.exe /nologo /fo build/payload.generated.res build/payload.generated.rc
    Assert-Exit 'Windows resource compilation'
    cl.exe /nologo /utf-8 /std:c++17 /EHsc /W4 /O2 /MT /Ibuild /Fobuild/ src/native/Patcher.cpp build/payload.generated.res /link /SUBSYSTEM:WINDOWS /MACHINE:X86 /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:src/native/Patcher.manifest /OUT:release/ShenzhenKoreanPatcher.exe user32.lib gdi32.lib shell32.lib ole32.lib uuid.lib bcrypt.lib
    Assert-Exit 'Win32 patcher build'
    py -3 tools/package.py --zip
    Assert-Exit 'Release packaging'
} finally {
    Pop-Location
    $env:Path = $oldPath
    $env:INCLUDE = $oldInclude
    $env:LIB = $oldLib
}
