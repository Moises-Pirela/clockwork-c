$ErrorActionPreference = 'Stop'
$sourcePath = Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party/sokol'
$requiredFiles = @('sokol_app.h','sokol_gfx.h','sokol_glue.h','sokol_log.h','util/sokol_gl.h','util/sokol_debugtext.h','LICENSE')
foreach ($fileName in $requiredFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $sourcePath $fileName))) {
        throw "Bundled Sokol file missing: $fileName. Restore third_party/sokol from the repository."
    }
}
return $sourcePath
