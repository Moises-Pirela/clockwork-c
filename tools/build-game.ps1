# Builds a game project against the engine on Windows (MSVC). Called by the
# editor (F7 / F8) and usable by hand from a Visual Studio developer prompt:
#
#   tools\build-game.ps1 <engine_root> <project.cwproj> module <output.dll> <source.c>...
#   tools\build-game.ps1 <engine_root> <project.cwproj> export <output.exe> <source.c>...
#
# module  compiles the game sources with the ECS into a DLL the editor and the
#         player load at run time.
# export  compiles a standalone game: the player with the module linked in,
#         plus game.cwproj, the scenes and the assets next to the executable.
param(
    [Parameter(Mandatory = $true)][string]$EngineRoot,
    [Parameter(Mandatory = $true)][string]$Project,
    [Parameter(Mandatory = $true)][ValidateSet('module', 'export')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Output,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Sources
)
$ErrorActionPreference = 'Stop'
if (!(Get-Command cl -ErrorAction SilentlyContinue)) { throw 'cl.exe was not found: run from a Visual Studio developer prompt (or start the editor from one).' }
$projectDir = Split-Path -Parent (Resolve-Path $Project)
$outDir = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$objDir = Join-Path $outDir 'obj'
New-Item -ItemType Directory -Force -Path $objDir | Out-Null
$engineSources = @('clockwork', 'cw_sim', 'cw_commands', 'cw_bytes', 'cw_snapshot', 'cw_file', 'cw_reflect', 'cw_scene', 'cw_game', 'cw_assets', 'cw_anim', 'cw_ui', 'cw_brush') | ForEach-Object { Join-Path $EngineRoot "src\$_.c" }
$flags = @('/nologo', '/std:c11', '/TC', '/W4', '/O2', "/I$EngineRoot\include", "/Fo$objDir\")
function Run([string[]]$command) {
    Write-Host "+ $($command -join ' ')"
    & $command[0] $command[1..($command.Length - 1)]
    if ($LASTEXITCODE -ne 0) { throw "command failed (exit $LASTEXITCODE)" }
}
switch ($Mode) {
    'module' {
        Run (@('cl') + $flags + @('/LD') + $engineSources + $Sources + @("/Fe:$Output"))
        Write-Host "module: $Output"
    }
    'export' {
        $sokol = Join-Path $EngineRoot 'third_party\sokol'
        $render2d = @('render2d', 'r2d_particles', 'r2d_effects', 'r2d_tunables', 'r2d_post_params', 'r2d_post', 'r2d_post_shaders', 'r2d_shadow', 'r2d_lighting', 'r2d_quality', 'r2d_image', 'r2d_material', 'r2d_sprite', 'r2d_sprite_shaders') | ForEach-Object { Join-Path $EngineRoot "render2d\$_.c" }
        $render3d = @('render3d', 'r3d_mesh', 'r3d_shaders') | ForEach-Object { Join-Path $EngineRoot "render3d\$_.c" }
        Run (@('cl', '/nologo', '/std:c11', '/TC', '/w', '/O1', '/DSOKOL_D3D11', "/I$sokol", '/c', (Join-Path $EngineRoot 'examples\sokol_impl.c'), "/Fo$objDir\sokol_impl.obj"))
        Run (@('cl') + $flags + @('/DSOKOL_D3D11', '/DCW_STATIC_GAME', "/I$EngineRoot\render2d", "/I$EngineRoot\render3d", "/I$EngineRoot\runtime", "/I$sokol",
            (Join-Path $EngineRoot 'runtime\player.c'), (Join-Path $EngineRoot 'runtime\host_canvas.c'), (Join-Path $EngineRoot 'runtime\host_view3d.c')) + $render2d + $render3d + $engineSources + $Sources +
            @("$objDir\sokol_impl.obj", "/Fe:$Output", '/link', 'd3d11.lib', 'dxgi.lib', 'user32.lib', 'gdi32.lib', 'shell32.lib', 'ole32.lib', '/INCREMENTAL:NO'))
        Copy-Item -Force $Project (Join-Path $outDir 'game.cwproj')
        $scenes = Join-Path $projectDir 'scenes'
        if (Test-Path $scenes) {
            $target = Join-Path $outDir 'scenes'
            if (Test-Path $target) { Remove-Item -Recurse -Force $target }
            Copy-Item -Recurse $scenes $target
        }
        $render = Join-Path $projectDir 'render.cfg'
        if (Test-Path $render) { Copy-Item -Force $render (Join-Path $outDir 'render.cfg') }
        $tunable = Join-Path $EngineRoot 'tunable.cfg'
        if (Test-Path $tunable) { Copy-Item -Force $tunable (Join-Path $outDir 'tunable.cfg') }
        # the assets: the manifest and every file it lists (images, materials, shaders, clips, prefabs...), same relative paths
        $manifestRel = 'assets.cwassets'
        foreach ($line in Get-Content $Project) { if ($line -match '^assets\s+(.+)$') { $manifestRel = $Matches[1].Trim(); break } }
        $manifest = Join-Path $projectDir $manifestRel
        $copied = 0
        if (Test-Path $manifest) {
            $manifestOut = Join-Path $outDir $manifestRel
            New-Item -ItemType Directory -Force (Split-Path $manifestOut) | Out-Null
            Copy-Item -Force $manifest $manifestOut
            foreach ($line in Get-Content $manifest) {
                if ($line -match '^asset [0-9a-fA-F]+ [a-z]+ "([^"]*)"') {
                    $rel = $Matches[1]
                    $copied++
                    if ($rel -match '\.(c|h)$') { continue }   # sources are compiled in
                    $src = Join-Path $projectDir $rel
                    if (Test-Path $src) {
                        $dst = Join-Path $outDir $rel
                        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
                        Copy-Item -Force $src $dst
                    }
                }
            }
        }
        Write-Host "exported: $Output (with game.cwproj, scenes\ and $copied assets)"
    }
}
