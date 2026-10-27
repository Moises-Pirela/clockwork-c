param(
    [ValidateSet('msvc', 'clang')][string]$Compiler = 'msvc',
    [switch]$Release,
    [switch]$Sanitize
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools are required. Alternatively, use CMake or compile the three C sources directly.' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'No Visual Studio C++ toolchain was found.' }
if ($Sanitize -and $Compiler -eq 'msvc') {
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.ASAN -property installationPath
    if (!$installation) { throw 'Install the Visual Studio C++ AddressSanitizer component.' }
}
$devcmd = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
$configuration = if ($Release) { 'release' } else { 'debug' }
$suffix = if ($Sanitize) { '-asan' } else { '' }
$out = Join-Path $PSScriptRoot "build/$Compiler-$configuration$suffix"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$flags = '/nologo /std:c11 /W4 /WX /TC /I"' + (Join-Path $PSScriptRoot 'include') + '"'
$flags += if ($Release) { ' /O2 /DNDEBUG' } else { ' /Od /Zi' }
if ($Sanitize) { $flags += ' /fsanitize=address' }
$cc = 'cl'
if ($Compiler -eq 'clang') {
    $clang = $null
    $installations = & $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    foreach ($candidate in $installations) {
        foreach ($relative in @('VC/Tools/Llvm/x64/bin/clang-cl.exe', 'VC/Tools/Llvm/bin/clang-cl.exe')) {
            $candidateClang = Join-Path $candidate $relative
            if (Test-Path -LiteralPath $candidateClang) { $clang = $candidateClang; break }
        }
        if ($clang) { $devcmd = Join-Path $candidate 'Common7/Tools/VsDevCmd.bat'; break }
    }
    if (!$clang) { throw 'Install the Visual Studio Clang compiler component.' }
    $cc = '"' + $clang + '"'
}
$commands = @(
    '@echo off',
    'set "TEMP=%CD%"',
    'set "TMP=%CD%"',
    ('call "' + $devcmd + '" -arch=x64 -host_arch=x64 >nul'),
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /c "' + (Join-Path $PSScriptRoot 'src/clockwork.c') + '" "' + (Join-Path $PSScriptRoot 'src/cw_sim.c') + '" "' + (Join-Path $PSScriptRoot 'src/cw_commands.c') + '"'),
    'if errorlevel 1 exit /b 1',
    'lib /nologo /out:clockwork.lib clockwork.obj cw_sim.obj cw_commands.obj',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' "' + (Join-Path $PSScriptRoot 'tests/test_clockwork.c') + '" /Fo:test_clockwork.obj /Fe:clockwork_tests.exe /link clockwork.lib /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_tests.exe',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' "' + (Join-Path $PSScriptRoot 'examples/movement.c') + '" /Fo:movement.obj /Fe:clockwork_movement.exe /link clockwork.lib /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_movement.exe',
    'exit /b %errorlevel%'
)
$batch = Join-Path $out 'compile.cmd'
Set-Content -LiteralPath $batch -Value $commands -Encoding ascii
Push-Location $out
try {
    & $env:ComSpec /d /c "`"$batch`""
    if ($LASTEXITCODE -ne 0) { throw "Build or tests failed (exit $LASTEXITCODE)." }
} finally { Pop-Location }
Write-Host "Built and tested $out"
