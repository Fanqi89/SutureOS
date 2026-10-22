# SutureOS: publish the GitHub release for the current version, ISO attached.
#
# Companion to scripts/release.ps1 (which commits + tags + pushes). This half
# creates or updates the GitHub *release* for tag v<version> and uploads the ISO,
# following the conventions the repository already uses:
#
#   title : "SutureOS v<version>: <中文概括> (<English descriptor>)"
#   body  : "## 🚀 本次更新 (prev → this)" + 🛠修复 / ✨新增 / ✅验证 /
#           📦下载与体验 / ⚠️已知限制   (see RELEASING.md)
#   asset : sutureos.iso
#
# Notes:
#  * the JSON payload is built by scripts/make_release_json.py - assembling it in
#    PowerShell inflated the body past GitHub's 125000-character limit (422);
#  * every API call goes through curl with --ssl-no-revoke: the local proxy
#    terminates TLS and schannel's revocation check fails otherwise;
#  * the token comes from $env:GITHUB_TOKEN, else from the git credential store
#    (the same credential the push uses).
#
# Usage:
#   powershell -NoProfile -File scripts\publish_release.ps1 `
#       -Name "SutureOS v0.6.0-alpha: 真机兼容与桌面可用 (Hypervisor & Desktop Update)" `
#       -NotesFile docs\release-notes-v0.6.0-alpha.md
param(
    [string]$Name      = "",
    [Parameter(Mandatory = $true)][string]$NotesFile,
    [string]$Iso       = "build\sutureos.iso",
    [string]$AssetName = "sutureos.iso",
    [switch]$Prerelease,
    [switch]$NoAsset,
    [string]$Token     = "",
    [string]$Proxy     = ""
)

$ErrorActionPreference = 'Stop'
$root      = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$owner     = 'Fanqi89'
$repoName  = 'SutureOS'

# ---------------------------------------------------------------- version ---
function Get-Version {
    $hdr = Join-Path $root 'include\sutureos\version.h'
    if (-not (Test-Path $hdr)) { throw "version header not found: $hdr" }
    $text = Get-Content $hdr -Raw
    function Def($key) {
        if ($text -match "#define\s+SUTUREOS_VERSION_$key\s+(\d+)") { return $Matches[1] }
        return '0'
    }
    $ver = "$(Def 'MAJOR').$(Def 'MINOR').$(Def 'PATCH')"
    if ($text -match '#define\s+SUTUREOS_VERSION_PRE\s+"([^"]+)"') {
        $pre = $Matches[1]
        if ($pre) { $ver = "$ver-$pre" }
    }
    return $ver
}

# ------------------------------------------------------------------ proxy ---
function Get-Proxy {
    if ($Proxy) { return $Proxy }
    $reg = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction SilentlyContinue
    if ($reg -and $reg.ProxyEnable -eq 1 -and $reg.ProxyServer) { return "http://$($reg.ProxyServer)" }
    return ""
}

# ------------------------------------------------------------------ token ---
function Get-Token {
    if ($Token) { return $Token }
    if ($env:GITHUB_TOKEN) { return $env:GITHUB_TOKEN }
    $out = "protocol=https`nhost=github.com`n`n" | & git.exe credential fill 2>&1
    $m = ($out | Select-String -Pattern '^password=(.+)$')
    if ($m) { return $m.Matches.Groups[1].Value }
    throw 'no GitHub token: set $env:GITHUB_TOKEN or store a credential for github.com'
}

$version   = Get-Version
$tag       = "v$version"
if (-not $Name) { $Name = "SutureOS $tag" }
$proxyUrl  = Get-Proxy
$tok       = Get-Token
$apiBase   = "https://api.github.com/repos/$owner/$repoName"

function Invoke-Api {
    param(
        [Parameter(Mandatory = $true)][string]$Method,
        [Parameter(Mandatory = $true)][string]$Uri,
        [string]$JsonFile = "",
        [string]$ContentType = "application/json; charset=utf-8"
    )
    $argv = @('-sS', '--ssl-no-revoke', '--fail-with-body', '-L', '-X', $Method)
    if ($proxyUrl) { $argv += @('--proxy', $proxyUrl) }
    $argv += @('-H', "Authorization: token $tok",
               '-H', 'User-Agent: SutureOS-publish-release',
               '-H', 'Accept: application/vnd.github+json',
               '-H', "Content-Type: $ContentType")
    if ($JsonFile) { $argv += @('--data-binary', "@$JsonFile") }
    $argv += @($Uri)
    $out = & curl.exe @argv 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "GitHub API $Method $Uri failed (curl exit $LASTEXITCODE): $out"
    }
    return ($out -join "`n")
}

if (-not (Test-Path (Join-Path $root $NotesFile))) { throw "notes file not found: $NotesFile" }

Write-Host "[publish] version $version (tag $tag)"
if ($proxyUrl) { Write-Host "[publish] proxy: $proxyUrl" }

# ---------------------------------------------------------------- payload ---
$payloadFile = Join-Path $env:TEMP 'sutureos-release.json'
$preArg = @()
if ($Prerelease) { $preArg = @('--prerelease') }
& py -3 (Join-Path $root 'scripts\make_release_json.py') `
    --tag $tag --name $Name --notes (Join-Path $root $NotesFile) --out $payloadFile @preArg
if ($LASTEXITCODE -ne 0) { throw 'could not build the release JSON payload (make_release_json.py)' }

# ---------------------------------------------------------------- release ---
$existing = $null
try {
    $existing = (Invoke-Api -Method GET -Uri "$apiBase/releases/tags/$tag") | ConvertFrom-Json
} catch {
    $existing = $null
}

if ($existing) {
    Write-Host "[publish] release exists (id=$($existing.id)) - updating title/notes"
    $rel = (Invoke-Api -Method PATCH -Uri "$apiBase/releases/$($existing.id)" -JsonFile $payloadFile) | ConvertFrom-Json
} else {
    Write-Host "[publish] creating release $tag"
    $rel = (Invoke-Api -Method POST -Uri "$apiBase/releases" -JsonFile $payloadFile) | ConvertFrom-Json
}
Write-Host "[publish] release: $($rel.html_url)"

# ------------------------------------------------------------------ asset ---
if (-not $NoAsset) {
    $isoPath = Join-Path $root $Iso
    if (-not (Test-Path $isoPath)) { throw "ISO not found: $isoPath (run scripts\build_iso.py first)" }
    $isoSize = (Get-Item $isoPath).Length
    $isoSha  = (Get-FileHash $isoPath -Algorithm SHA256).Hash.ToLower()
    Write-Host ("[publish] ISO {0}: {1:N0} bytes, sha256 {2}" -f $AssetName, $isoSize, $isoSha)

    # the asset name is stable across releases, so replace any existing one
    $relNow = (Invoke-Api -Method GET -Uri "$apiBase/releases/$($rel.id)") | ConvertFrom-Json
    foreach ($a in $relNow.assets) {
        if ($a.name -eq $AssetName) {
            Write-Host "[publish] removing the previous '$AssetName' (asset id=$($a.id))"
            Invoke-Api -Method DELETE -Uri "$apiBase/releases/assets/$($a.id)" | Out-Null
        }
    }

    $uploadUrl = "https://uploads.github.com/repos/$owner/$repoName/releases/$($rel.id)/assets?name=$AssetName"
    $argv = @('-sS', '--ssl-no-revoke', '--fail-with-body', '-L', '-X', 'POST')
    if ($proxyUrl) { $argv += @('--proxy', $proxyUrl) }
    $argv += @('-H', "Authorization: token $tok",
               '-H', 'User-Agent: SutureOS-publish-release',
               '-H', 'Content-Type: application/octet-stream',
               '--data-binary', "@$isoPath",
               $uploadUrl)
    $json = (& curl.exe @argv 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "asset upload failed (curl exit $LASTEXITCODE): $json" }
    $asset = $json | ConvertFrom-Json
    Write-Host "[publish] uploaded: $($asset.name) ($([math]::Round($asset.size / 1MB, 2)) MB)"
    Write-Host "[publish] download: $($asset.browser_download_url)"
    Write-Host "[publish] sha256  : $isoSha"
}

Write-Host "[publish] done: $($rel.html_url)"
