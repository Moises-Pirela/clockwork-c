param([string]$CacheRoot)
$ErrorActionPreference = 'Stop'
if (!$CacheRoot) { $CacheRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'build/dependencies' }
$CacheRoot = [System.IO.Path]::GetFullPath($CacheRoot)
$dependencyLock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'sokol.lock')
$dependencyUrl = $dependencyLock[0].Trim()
$expectedHash = $dependencyLock[1].Trim()
$revision = ($dependencyUrl -split '/')[-1]
$sourcePath = Join-Path $CacheRoot ('sokol-' + $revision)
$markerPath = Join-Path $sourcePath '.clockwork-ready'
$requiredFiles = @('sokol_app.h','sokol_gfx.h','sokol_glue.h','sokol_log.h','util/sokol_gl.h','util/sokol_debugtext.h','LICENSE')
$complete = $true
foreach ($fileName in $requiredFiles) { if (!(Test-Path -LiteralPath (Join-Path $sourcePath $fileName))) { $complete = $false } }
if ($complete -and (Test-Path -LiteralPath $markerPath) -and
    (Get-Content -LiteralPath $markerPath -Raw).Trim() -eq $expectedHash) { return $sourcePath }
New-Item -ItemType Directory -Force -Path $CacheRoot | Out-Null
$archivePath = Join-Path $CacheRoot ('sokol-' + $revision + '.zip')
if (!(Test-Path -LiteralPath $archivePath)) {
    $temporaryArchive = $archivePath + '.download'
    Invoke-WebRequest -Uri $dependencyUrl -OutFile $temporaryArchive
    if ((Get-FileHash -LiteralPath $temporaryArchive -Algorithm SHA256).Hash -ne $expectedHash) {
        Remove-Item -LiteralPath $temporaryArchive -Force
        throw 'Sokol dependency checksum mismatch.'
    }
    Move-Item -LiteralPath $temporaryArchive -Destination $archivePath -Force
}
if ((Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash -ne $expectedHash) {
    throw 'Sokol dependency checksum mismatch; remove the cached archive and retry.'
}
& tar -xf $archivePath -C $CacheRoot
if ($LASTEXITCODE -ne 0) { throw 'Sokol dependency extraction failed.' }
foreach ($fileName in $requiredFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $sourcePath $fileName))) { throw "Sokol dependency missing $fileName" }
}
[System.IO.File]::WriteAllText($markerPath, $expectedHash)
return $sourcePath
