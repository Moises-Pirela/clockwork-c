param(
    [ValidateSet('msvc', 'clang')][string]$Compiler = 'msvc',
    [switch]$Release,
    [switch]$Sanitize,
    [switch]$NoGui
)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ Build Tools are required. Alternatively, use CMake or compile the C sources in src directly.' }
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
$sokolHeaders = & (Join-Path $PSScriptRoot 'tools/prepare-sokol.ps1')
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
    ($cc + ' ' + $flags + ' /c "' + (Join-Path $PSScriptRoot 'src/clockwork.c') + '" "' + (Join-Path $PSScriptRoot 'src/cw_sim.c') + '" "' + (Join-Path $PSScriptRoot 'src/cw_commands.c') + '" "' + (Join-Path $PSScriptRoot 'src/cw_bytes.c') + '" "' + (Join-Path $PSScriptRoot 'src/cw_snapshot.c') + '" "' + (Join-Path $PSScriptRoot 'src/cw_file.c') + '"'),
    'if errorlevel 1 exit /b 1',
    'lib /nologo /out:clockwork.lib clockwork.obj cw_sim.obj cw_commands.obj cw_bytes.obj cw_snapshot.obj cw_file.obj',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' "' + (Join-Path $PSScriptRoot 'tests/test_clockwork.c') + '" /Fo:test_clockwork.obj /Fe:clockwork_tests.exe /link clockwork.lib /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_tests.exe',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' "' + (Join-Path $PSScriptRoot 'examples/movement.c') + '" /Fo:movement.obj /Fe:clockwork_movement.exe /link clockwork.lib /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_movement.exe',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /I"' + (Join-Path $PSScriptRoot 'render2d') + '" /I"' + $sokolHeaders + '" "' + (Join-Path $PSScriptRoot 'tests/test_render2d.c') + '" /Fo:test_render2d.obj /Fe:clockwork_render2d_tests.exe /link /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_render2d_tests.exe',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /I"' + (Join-Path $PSScriptRoot 'render2d') + '" /I"' + $sokolHeaders + '" "' + (Join-Path $PSScriptRoot 'tests/test_r2d_fx.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_particles.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_effects.c') + '" /Fe:clockwork_r2d_fx_tests.exe /link /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_r2d_fx_tests.exe',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /I"' + (Join-Path $PSScriptRoot 'render2d') + '" /I"' + $sokolHeaders + '" "' + (Join-Path $PSScriptRoot 'tests/test_r2d_post.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_post_params.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_tunables.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_shadow.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_quality.c') + '" /Fe:clockwork_r2d_post_tests.exe /link /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_r2d_post_tests.exe',
    'if errorlevel 1 exit /b 1',
    'exit /b %errorlevel%'
)
if (!$NoGui) {
    $sokol = $sokolHeaders
    $sokolFlags = '/nologo /std:c11 /TC /w /DSOKOL_D3D11 /I"' + $sokol + '"'
    $sokolFlags += if ($Release) { ' /O2' } else { ' /O1' }
    $commands = $commands[0..($commands.Length - 2)] + @(
        ($cc + ' ' + $sokolFlags + ' /c "' + (Join-Path $PSScriptRoot 'examples/sokol_impl.c') + '" /Fo:sokol_impl.obj'),
        'if errorlevel 1 exit /b 1',
        'lib /nologo /out:clockwork_sokol.lib sokol_impl.obj',
        'if errorlevel 1 exit /b 1',
        ($cc + ' ' + $flags + ' /DSOKOL_D3D11 /I"' + $sokol + '" "' + (Join-Path $PSScriptRoot 'examples/save_load.c') + '" /Fo:save_load.obj /Fe:clockwork_save_load.exe /link clockwork.lib clockwork_sokol.lib d3d11.lib dxgi.lib user32.lib gdi32.lib shell32.lib ole32.lib /INCREMENTAL:NO'),
        'if errorlevel 1 exit /b 1',
        'clockwork_save_load.exe --headless',
        'if errorlevel 1 exit /b 1',
        ($cc + ' ' + $flags + ' /DSOKOL_D3D11 /I"' + (Join-Path $PSScriptRoot 'render2d') + '" /I"' + $sokol + '" /c "' + (Join-Path $PSScriptRoot 'render2d/render2d.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_particles.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_effects.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_tunables.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_post_params.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_post.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_post_shaders.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_shadow.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_lighting.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_quality.c') + '"'),
        'if errorlevel 1 exit /b 1',
        'lib /nologo /out:clockwork_render2d.lib render2d.obj r2d_particles.obj r2d_effects.obj r2d_tunables.obj r2d_post_params.obj r2d_post.obj r2d_post_shaders.obj r2d_shadow.obj r2d_lighting.obj r2d_quality.obj',
        ('copy /Y "' + (Join-Path $PSScriptRoot 'tunable.cfg') + '" tunable.cfg >nul'),
        'if errorlevel 1 exit /b 1',
        ($cc + ' ' + $flags + ' /DSOKOL_D3D11 /I"' + (Join-Path $PSScriptRoot 'render2d') + '" /I"' + $sokol + '" "' + (Join-Path $PSScriptRoot 'examples/combat.c') + '" /Fo:combat.obj /Fe:clockwork_combat.exe /link clockwork.lib clockwork_render2d.lib clockwork_sokol.lib d3d11.lib dxgi.lib user32.lib gdi32.lib shell32.lib ole32.lib /INCREMENTAL:NO'),
        'if errorlevel 1 exit /b 1',
        'clockwork_combat.exe --headless combat_test.cwsn',
        'exit /b %errorlevel%'
    )
}
$batch = Join-Path $out 'compile.cmd'
Set-Content -LiteralPath $batch -Value $commands -Encoding ascii
Push-Location $out
try {
    & $env:ComSpec /d /c "`"$batch`""
    if ($LASTEXITCODE -ne 0) { throw "Build or tests failed (exit $LASTEXITCODE)." }
} finally { Pop-Location }
Write-Host "Built and tested $out"
