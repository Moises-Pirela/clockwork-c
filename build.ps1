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
$engineSources = @('clockwork', 'cw_sim', 'cw_commands', 'cw_bytes', 'cw_snapshot', 'cw_file', 'cw_reflect', 'cw_scene', 'cw_game', 'cw_assets', 'cw_anim', 'cw_ui')
$engineFiles = ($engineSources | ForEach-Object { '"' + (Join-Path $PSScriptRoot "src/$_.c") + '"' }) -join ' '
$engineObjects = ($engineSources | ForEach-Object { "$_.obj" }) -join ' '
$combatProject = Join-Path $PSScriptRoot 'games/combat/combat.cwproj'
$combatModule = Join-Path $PSScriptRoot 'games/combat/build/combat_game.dll'
$orbitProject = Join-Path $PSScriptRoot 'games/orbit/orbit.cwproj'
$orbitModule = Join-Path $PSScriptRoot 'games/orbit/build/orbit_game.dll'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $combatModule) | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $orbitModule) | Out-Null
$commands = @(
    '@echo off',
    'set "TEMP=%CD%"',
    'set "TMP=%CD%"',
    ('call "' + $devcmd + '" -arch=x64 -host_arch=x64 >nul'),
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /c ' + $engineFiles),
    'if errorlevel 1 exit /b 1',
    ('lib /nologo /out:clockwork.lib ' + $engineObjects),
    'if errorlevel 1 exit /b 1'
)
if (Test-Path (Join-Path $PSScriptRoot 'tests/test_clockwork.c')) {
    $commands += @(
        ($cc + ' ' + $flags + ' "' + (Join-Path $PSScriptRoot 'tests/test_clockwork.c') + '" /Fo:test_clockwork.obj /Fe:clockwork_tests.exe /link clockwork.lib /INCREMENTAL:NO'),
        'if errorlevel 1 exit /b 1',
        'clockwork_tests.exe',
        'if errorlevel 1 exit /b 1'
    )
}
$commands += @(
    # game modules as DLLs at the paths their projects name, then the engine-level and editor checks through them
    ($cc + ' ' + $flags + ' /LD ' + $engineFiles + ' "' + (Join-Path $PSScriptRoot 'games/combat/combat_game.c') + '" /Fe:"' + $combatModule + '"'),
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /LD ' + $engineFiles + ' "' + (Join-Path $PSScriptRoot 'games/orbit/orbit_game.c') + '" /Fe:"' + $orbitModule + '"'),
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' "' + (Join-Path $PSScriptRoot 'tests/test_game.c') + '" "' + (Join-Path $PSScriptRoot 'games/combat/combat_game.c') + '" /Fe:clockwork_game_tests.exe /link clockwork.lib /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    ('clockwork_game_tests.exe "' + $combatModule + '"'),
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /I"' + (Join-Path $PSScriptRoot 'editor') + '" /I"' + (Join-Path $PSScriptRoot 'render2d') + '" "' + (Join-Path $PSScriptRoot 'tests/test_editor.c') + '" "' + (Join-Path $PSScriptRoot 'editor/editor_core.c') + '" "' + (Join-Path $PSScriptRoot 'editor/editor_settings.c') + '" "' + (Join-Path $PSScriptRoot 'editor/editor_anim.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_tunables.c') + '" /Fe:clockwork_editor_tests.exe /link clockwork.lib /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    ('clockwork_editor_tests.exe "' + $combatProject + '" "' + $PSScriptRoot + '" "' + $orbitProject + '"'),
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /LD ' + $engineFiles + ' "' + (Join-Path $PSScriptRoot 'launcher/template/game.c') + '" /Fe:starter_template.dll'),
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /I"' + (Join-Path $PSScriptRoot 'launcher') + '" "' + (Join-Path $PSScriptRoot 'tests/test_launcher.c') + '" "' + (Join-Path $PSScriptRoot 'launcher/launcher_core.c') + '" /Fe:clockwork_launcher_tests.exe /link clockwork.lib /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    ('clockwork_launcher_tests.exe "' + $PSScriptRoot + '" "' + $out + '"'),
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /I"' + (Join-Path $PSScriptRoot 'render2d') + '" /I"' + $sokolHeaders + '" "' + (Join-Path $PSScriptRoot 'tests/test_render2d.c') + '" /Fo:test_render2d.obj /Fe:clockwork_render2d_tests.exe /link /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_render2d_tests.exe',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /I"' + (Join-Path $PSScriptRoot 'render2d') + '" "' + (Join-Path $PSScriptRoot 'tests/test_image.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_image.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_material.c') + '" /Fe:clockwork_image_tests.exe /link /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_image_tests.exe',
    'if errorlevel 1 exit /b 1',
    ($cc + ' ' + $flags + ' /I"' + (Join-Path $PSScriptRoot 'runtime') + '" /I"' + (Join-Path $PSScriptRoot 'render2d') + '" /I"' + $sokolHeaders + '" "' + (Join-Path $PSScriptRoot 'tests/test_dock.c') + '" "' + (Join-Path $PSScriptRoot 'runtime/host_dock.c') + '" /Fe:clockwork_dock_tests.exe /link /INCREMENTAL:NO'),
    'if errorlevel 1 exit /b 1',
    'clockwork_dock_tests.exe',
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
    $guiIncludes = ' /DSOKOL_D3D11 /I"' + (Join-Path $PSScriptRoot 'render2d') + '" /I"' + (Join-Path $PSScriptRoot 'runtime') + '" /I"' + $sokol + '"'
    $guiLibs = 'clockwork.lib clockwork_render2d.lib clockwork_sokol.lib d3d11.lib dxgi.lib user32.lib gdi32.lib shell32.lib ole32.lib /INCREMENTAL:NO'
    $commands = $commands[0..($commands.Length - 2)] + @(
        ($cc + ' ' + $sokolFlags + ' /c "' + (Join-Path $PSScriptRoot 'examples/sokol_impl.c') + '" /Fo:sokol_impl.obj'),
        'if errorlevel 1 exit /b 1',
        'lib /nologo /out:clockwork_sokol.lib sokol_impl.obj',
        'if errorlevel 1 exit /b 1',
        ($cc + ' ' + $flags + $guiIncludes + ' /c "' + (Join-Path $PSScriptRoot 'render2d/render2d.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_particles.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_effects.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_tunables.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_post_params.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_post.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_post_shaders.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_shadow.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_lighting.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_quality.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_image.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_material.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_sprite.c') + '" "' + (Join-Path $PSScriptRoot 'render2d/r2d_sprite_shaders.c') + '" "' + (Join-Path $PSScriptRoot 'runtime/host_canvas.c') + '" "' + (Join-Path $PSScriptRoot 'runtime/host_ui.c') + '" "' + (Join-Path $PSScriptRoot 'runtime/host_dock.c') + '"'),
        'if errorlevel 1 exit /b 1',
        'lib /nologo /out:clockwork_render2d.lib render2d.obj r2d_particles.obj r2d_effects.obj r2d_tunables.obj r2d_post_params.obj r2d_post.obj r2d_post_shaders.obj r2d_shadow.obj r2d_lighting.obj r2d_quality.obj r2d_image.obj r2d_material.obj r2d_sprite.obj r2d_sprite_shaders.obj host_canvas.obj host_ui.obj host_dock.obj',
        ('copy /Y "' + (Join-Path $PSScriptRoot 'tunable.cfg') + '" tunable.cfg >nul'),
        'if errorlevel 1 exit /b 1',
        # the player: runs any module on a scene
        ($cc + ' ' + $flags + $guiIncludes + ' "' + (Join-Path $PSScriptRoot 'runtime/player.c') + '" /Fo:player.obj /Fe:clockwork_player.exe /link ' + $guiLibs),
        'if errorlevel 1 exit /b 1',
        ('clockwork_player.exe --project "' + $combatProject + '" --headless 120'),
        'if errorlevel 1 exit /b 1',
        ('clockwork_player.exe --project "' + $orbitProject + '" --headless 300'),
        'if errorlevel 1 exit /b 1',
        # the editor: opens projects, edits scenes, builds games
        ($cc + ' ' + $flags + $guiIncludes + ' /DCW_ENGINE_ROOT=\"' + ($PSScriptRoot -replace '\\', '\\\\') + '\" /I"' + (Join-Path $PSScriptRoot 'editor') + '" "' + (Join-Path $PSScriptRoot 'editor/editor_main.c') + '" "' + (Join-Path $PSScriptRoot 'editor/editor_core.c') + '" "' + (Join-Path $PSScriptRoot 'editor/editor_settings.c') + '" "' + (Join-Path $PSScriptRoot 'editor/editor_anim.c') + '" /Fe:clockwork_editor.exe /link ' + $guiLibs),
        'if errorlevel 1 exit /b 1',
        # the launcher: lists, creates, builds and opens projects
        ($cc + ' ' + $flags + $guiIncludes + ' /DCW_ENGINE_ROOT=\"' + ($PSScriptRoot -replace '\\', '\\\\') + '\" /I"' + (Join-Path $PSScriptRoot 'launcher') + '" "' + (Join-Path $PSScriptRoot 'launcher/launcher_main.c') + '" "' + (Join-Path $PSScriptRoot 'launcher/launcher_core.c') + '" /Fe:clockwork_launcher.exe /link ' + $guiLibs),
        'if errorlevel 1 exit /b 1',
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
if (!$NoGui) { Write-Host "Run $out\clockwork_launcher.exe to create and open projects, $out\clockwork_editor.exe --project games\combat\combat.cwproj for the editor, $out\clockwork_player.exe --project games\combat\combat.cwproj for the game." }
