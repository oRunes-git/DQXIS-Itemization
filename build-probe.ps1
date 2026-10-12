param([string]$BuildDirectory = 'build')
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$installation = & 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Microsoft C++ tools are missing.' }
$vcvarsPath = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$buildRoot = Join-Path $projectRoot $BuildDirectory
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$commands = @(
    '@echo off',
    ('call "' + $vcvarsPath + '"'),
    ('cd /d "' + $projectRoot + '"'),
    ('cl /nologo /LD /std:c++17 /EHsc /W4 /O2 /MT /I"include" /I"third_party" /Fo"' + $buildRoot + '/" "src\native_probe.cpp" "third_party\MinHook\buffer.c" "third_party\MinHook\hook.c" "third_party\MinHook\trampoline.c" "third_party\MinHook\hde\hde64.c" /link /OUT:"' + $buildRoot + '\itemization-probe.dll"'),
    'if errorlevel 1 exit /b 1',
    ('cl /nologo /std:c++17 /EHsc /W4 /O2 /MT "src\load_probe.cpp" /Fo"' + $buildRoot + '\load_probe.obj" /Fe:"' + $buildRoot + '\load-probe.exe"'),
    'if errorlevel 1 exit /b 1'
)
$batchPath = Join-Path $buildRoot 'compile-probe.cmd'
[IO.File]::WriteAllLines($batchPath,$commands,[Text.Encoding]::ASCII)
& cmd.exe /d /c ('"' + $batchPath + '"')
if ($LASTEXITCODE -ne 0) { throw 'Diagnostic build failed.' }
