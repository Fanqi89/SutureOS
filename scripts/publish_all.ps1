# SutureOS: ONE command that does the whole release.
#
# The release discipline (see docs/RELEASING.md) has two halves:
#   1) commit + annotated tag v<version> + push main/tags   (scripts/release.ps1)
#   2) GitHub Release with title/notes + sutureos.iso asset (scripts/publish_release.ps1)
#
# Splitting them means the ISO can be forgotten, so this wrapper runs both and
# stops on the first failure. It is idempotent: an existing tag is left alone,
# an existing release is updated, and the ISO asset is replaced.
#
# Usage:
#   powershell -NoProfile -File scripts\publish_all.ps1 `
#       -Message "feat: ..." `
#       -TagNotes build\tag-notes-v0.6.1-alpha.txt `
#       -ReleaseName "SutureOS v0.6.1-alpha: <中文概括> (<English descriptor>)" `
#       -ReleaseNotes docs\release-notes-v0.6.1-alpha.md
#
#   # publish only (already committed + tagged):
#   powershell -NoProfile -File scripts\publish_all.ps1 -PushOnly `
#       -ReleaseName "..." -ReleaseNotes docs\release-notes-....md
param(
    [string]$Message       = "",
    [string]$TagNotes      = "",
    [string]$ReleaseName   = "",
    [Parameter(Mandatory = $true)][string]$ReleaseNotes,
    [string]$Iso           = "build\sutureos.iso",
    [switch]$Prerelease,
    [switch]$PushOnly,
    [switch]$NoAsset,
    [string]$Proxy         = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$repo = 'https://github.com/Fanqi89/SutureOS'

# version is the single source of truth
$hdr = Get-Content (Join-Path $root 'include\sutureos\version.h') -Raw
function Def($key) {
    if ($hdr -match "#define\s+SUTUREOS_VERSION_$key\s+(\d+)") { return $Matches[1] }
    return '0'
}
$version = "$(Def 'MAJOR').$(Def 'MINOR').$(Def 'PATCH')"
if ($hdr -match '#define\s+SUTUREOS_VERSION_PRE\s+"([^"]+)"') {
    $pre = $Matches[1]
    if ($pre) { $version = "$version-$pre" }
}
$tag = "v$version"
Write-Host "=============================================================="
Write-Host " SutureOS release $version (tag $tag)"
Write-Host "=============================================================="

# ---- step 0: snapshot the existing releases BEFORE touching anything --------
# (without a pre-publish snapshot you cannot prove that the upload did not
#  clobber an older release)
$snapArgs = @('-SaveBaseline')
if ($Proxy) { $snapArgs += @('-Proxy', $Proxy) }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\verify_release.ps1') @snapArgs
if ($LASTEXITCODE -ne 0) { throw "could not snapshot the existing releases" }

# ---- half 1: commit + tag + push -------------------------------------------
$relArgs = @('-PushOnly')
if (-not $PushOnly) {
    if (-not $Message)   { throw "-Message is required unless -PushOnly is given" }
    if (-not $TagNotes)  { throw "-TagNotes is required unless -PushOnly is given (what this version does)" }
    $relArgs = @('-Message', $Message, '-NotesFile', $TagNotes)
}
if ($Proxy) { $relArgs += @('-Proxy', $Proxy) }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\release.ps1') @relArgs
if ($LASTEXITCODE -ne 0) { throw "release.ps1 failed (exit $LASTEXITCODE) - stopping before the GitHub release" }

# ---- half 2: GitHub Release + ISO asset ------------------------------------
if (-not $ReleaseName) { $ReleaseName = "SutureOS $tag" }
$pubArgs = @('-Name', $ReleaseName, '-NotesFile', $ReleaseNotes, '-Iso', $Iso)
if ($Prerelease) { $pubArgs += '-Prerelease' }
if ($NoAsset)    { $pubArgs += '-NoAsset' }
if ($Proxy)      { $pubArgs += @('-Proxy', $Proxy) }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\publish_release.ps1') @pubArgs
if ($LASTEXITCODE -ne 0) { throw "publish_release.ps1 failed (exit $LASTEXITCODE)" }

# ---- step 3: prove it (version inside the ISO, asset digest, old releases) --
$verifyArgs = @()
if ($Proxy) { $verifyArgs += @('-Proxy', $Proxy) }
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\verify_release.ps1') @verifyArgs
if ($LASTEXITCODE -ne 0) { throw "release verification FAILED - do not announce this release" }

Write-Host ""
Write-Host "=============================================================="
Write-Host " released $tag"
Write-Host "   commits : $repo/commits/main"
Write-Host "   tags    : $repo/tags"
Write-Host "   release : $repo/releases/tag/$tag"
Write-Host "   ISO     : $repo/releases/download/$tag/sutureos.iso"
Write-Host "=============================================================="
