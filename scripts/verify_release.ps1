# SutureOS: prove a release is real (not just "the upload said ok").
#
# Implements the binary-release verification checklist for this repository:
#   1. the version string from include/sutureos/version.h is actually INSIDE the
#      built ISO (and the previous version string is gone),
#   2. the local ISO size + sha256 are recorded,
#   3. GET-read the Release back and match the asset's size + digest by NAME
#      (the API response order is not stable, never compare by index),
#   4. download the asset back and recompute sha256 -> three-way agreement
#      (local == API digest == downloaded file),
#   5. every OLD Release is unchanged compared with the pre-publish snapshot,
#   6. the git side is in sync (HEAD == origin/main, no divergence, clean tree).
#
# Usage:
#   # before publishing: snapshot every existing Release
#   powershell -NoProfile -File scripts\verify_release.ps1 -SaveBaseline
#   # after publishing: full verification
#   powershell -NoProfile -File scripts\verify_release.ps1
#   powershell -NoProfile -File scripts\verify_release.ps1 -DownloadBack  # also pull the bytes back
param(
    [string]$Iso        = "build\sutureos.iso",
    [string]$AssetName  = "sutureos.iso",
    [string]$Baseline   = "build\release-baseline.json",
    [switch]$SaveBaseline,
    [switch]$DownloadBack,
    [string]$Token      = "",
    [string]$Proxy      = ""
)

$ErrorActionPreference = 'Stop'
$root  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$owner = 'Fanqi89'
$repoName = 'SutureOS'

$script:checks = 0
$script:failed = 0
function Check([string]$what, [bool]$ok, [string]$detail = "") {
    $script:checks++
    if ($ok) {
        Write-Host ("  [ ok ] {0}{1}" -f $what, $(if ($detail) { "  ($detail)" } else { "" }))
    } else {
        $script:failed++
        Write-Host ("  [FAIL] {0}{1}" -f $what, $(if ($detail) { "  ($detail)" } else { "" }))
    }
}

# ---------------------------------------------------------------- version ---
$hdr = Get-Content (Join-Path $root 'include\sutureos\version.h') -Raw
function Def($key) {
    if ($hdr -match "#define\s+SUTUREOS_VERSION_$key\s+(\d+)") { return $Matches[1] }
    return '0'
}
$version = "$(Def 'MAJOR').$(Def 'MINOR').$(Def 'PATCH')"
if ($hdr -match '#define\s+SUTUREOS_VERSION_PRE\s+"([^"]+)"') {
    $pre = $Matches[1]; if ($pre) { $version = "$version-$pre" }
}
$tag = "v$version"
$prePublish = [bool]$SaveBaseline   # snapshot mode: nothing is published yet

function Get-Proxy {
    if ($Proxy) { return $Proxy }
    $reg = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction SilentlyContinue
    if ($reg -and $reg.ProxyEnable -eq 1 -and $reg.ProxyServer) { return "http://$($reg.ProxyServer)" }
    return ""
}
function Get-Token {
    if ($Token) { return $Token }
    if ($env:GITHUB_TOKEN) { return $env:GITHUB_TOKEN }
    $out = "protocol=https`nhost=github.com`n`n" | & git.exe credential fill 2>&1
    $m = ($out | Select-String -Pattern '^password=(.+)$')
    if ($m) { return $m.Matches.Groups[1].Value }
    throw 'no GitHub token'
}

$proxyUrl = Get-Proxy
$tok = Get-Token
$apiBase = "https://api.github.com/repos/$owner/$repoName"

function Invoke-Api([string]$Uri) {
    $argv = @('-sS', '--ssl-no-revoke', '--fail-with-body', '-L')
    if ($proxyUrl) { $argv += @('--proxy', $proxyUrl) }
    $argv += @('-H', "Authorization: token $tok",
               '-H', 'User-Agent: SutureOS-verify-release',
               '-H', 'Accept: application/vnd.github+json', $Uri)
    $out = (& curl.exe @argv 2>&1) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "API GET $Uri failed: $out" }
    return ($out | ConvertFrom-Json)
}

Write-Host "[verify] $tag"
if ($proxyUrl) { Write-Host "[verify] proxy: $proxyUrl" }

# 1) version string must be inside the artifact, the previous one must be gone
$isoPath = Join-Path $root $Iso
if (-not (Test-Path $isoPath)) { throw "ISO not found: $isoPath (run scripts\build_iso.py)" }
$isoItem = Get-Item $isoPath
$isoSha = (Get-FileHash $isoPath -Algorithm SHA256).Hash.ToLower()
$bytes = [System.IO.File]::ReadAllBytes($isoPath)
function Count-Ascii([byte[]]$hay, [string]$needle) {
    $n = [System.Text.Encoding]::ASCII.GetBytes($needle)
    $hits = 0
    $limit = $hay.Length - $n.Length
    for ($i = 0; $i -le $limit; $i++) {
        if ($hay[$i] -ne $n[0]) { continue }
        $match = $true
        for ($j = 1; $j -lt $n.Length; $j++) {
            if ($hay[$i + $j] -ne $n[$j]) { $match = $false; break }
        }
        if ($match) { $hits++ }
    }
    return $hits
}
$hitNew = Count-Ascii $bytes $version
Check "version string '$version' inside $AssetName" ($hitNew -gt 0) "hits=$hitNew"

# previous tag -> its string must not appear any more
$allTags = @(& git.exe -C $root tag -l | Sort-Object)
$prevTag = $null
foreach ($t in $allTags) { if ($t -ne $tag) { $prevTag = $t } }
if ($prevTag) {
    $prevVer = $prevTag.TrimStart('v')
    $hitOld = Count-Ascii $bytes $prevVer
    Check "previous version string '$prevVer' is gone" ($hitOld -eq 0) "hits=$hitOld"
}
Write-Host ("[verify] local ISO: {0:N0} bytes, sha256 {1}" -f $isoItem.Length, $isoSha)

# 2) API: the release and its asset, matched BY NAME
#    (before publishing, the release for this tag does not exist yet - that is
#     expected when taking the pre-publish snapshot)
$rel = $null
try {
    $rel = Invoke-Api "$apiBase/releases/tags/$tag"
} catch {
    $rel = $null
}
if ($rel) {
    Check "release exists for $tag" $true "id=$($rel.id)"
} elseif ($SaveBaseline) {
    Write-Host "[verify] release for $tag not published yet (pre-publish snapshot)"
} else {
    Check "release exists for $tag" $false "not found"
}
$asset = if ($rel) { $rel.assets | Where-Object { $_.name -eq $AssetName } } else { $null }
if ($prePublish) {
    Write-Host "[skip] release/asset checks (pre-publish snapshot)"
} elseif ($null -eq $asset) {
    Check "release carries asset '$AssetName'" $false "assets=$($rel.assets.Count)"
} elseif ($asset) {
    Check "release carries asset '$AssetName'" $true "assets=$($rel.assets.Count)"
    Check "asset size == local ISO size" ([int64]$asset.size -eq $isoItem.Length) "$($asset.size) vs $($isoItem.Length)"
    Check "asset digest == local sha256" ($asset.digest -eq "sha256:$isoSha") $asset.digest
    Write-Host "[verify] download: $($asset.browser_download_url)"

    # 3) optionally download it back and recompute -> three-way agreement
        # The API size+digest read-back above is the authoritative proof. Pulling
        # the bytes back is optional because this proxy is slow for big reads
        # (measured: 256 KB range = 3.7 s, 1 MB = 52 s, 4 MB = timeout), so
        # -DownloadBack walks it in 256 KB ranges and tolerates network failure.
        if ($DownloadBack) {
            $tmp = Join-Path $env:TEMP ("sutureos-verify-" + $AssetName)
            if (Test-Path $tmp) { Remove-Item $tmp -Force }
            $total = [int64]$asset.size
            $chunkSize = 256KB
            $url = "$apiBase/releases/assets/$($asset.id)"
            $fs = [System.IO.File]::Create($tmp)
            $done = [int64]0
            $networkOk = $true
            try {
                while ($done -lt $total) {
                    $last = [Math]::Min($done + $chunkSize - 1, $total - 1)
                    $part = Join-Path $env:TEMP 'sutureos-verify-part.bin'
                    if (Test-Path $part) { Remove-Item $part -Force }
                    $argv = @('-sS', '--ssl-no-revoke', '--fail-with-body', '-L',
                              '--max-time', '120', '--range', "$done-$last", '-o', $part)
                    if ($proxyUrl) { $argv += @('--proxy', $proxyUrl) }
                    $argv += @('-H', "Authorization: token $tok",
                               '-H', 'Accept: application/octet-stream', $url)
                    & curl.exe @argv 2>&1 | Out-Null
                    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $part)) { $networkOk = $false; break }
                    $buf = [System.IO.File]::ReadAllBytes($part)
                    if ($buf.Length -eq 0) { $networkOk = $false; break }
                    $fs.Write($buf, 0, $buf.Length)
                    $done += $buf.Length
                    Remove-Item $part -Force -ErrorAction SilentlyContinue
                }
            } finally {
                $fs.Close()
            }
            if (-not $networkOk -or $done -ne $total) {
                Write-Host ("  [warn] asset read-back incomplete through the proxy " +
                            "($done/$total bytes) - the API digest remains the proof")
                Remove-Item $tmp -Force -ErrorAction SilentlyContinue
            } else {
                $dlSha = (Get-FileHash $tmp -Algorithm SHA256).Hash.ToLower()
                Check "downloaded asset sha256 == local sha256" ($dlSha -eq $isoSha) $dlSha
                Remove-Item $tmp -Force -ErrorAction SilentlyContinue
            }
        } else {
            Write-Host "  [skip] download-back (use -DownloadBack for the three-way check)"
        }
}

# 4) old releases must be untouched (compare with the pre-publish snapshot)
$baselinePath = Join-Path $root $Baseline
$all = @(Invoke-Api "$apiBase/releases?per_page=100")
if ($SaveBaseline) {
    $snap = $all | ForEach-Object {
        [pscustomobject]@{
            tag = $_.tag_name
            assets = @($_.assets | ForEach-Object { [pscustomobject]@{ name = $_.name; id = $_.id; size = $_.size; digest = $_.digest } })
        }
    }
    $dir = Split-Path -Parent $baselinePath
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    [System.IO.File]::WriteAllText($baselinePath,
        ($snap | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "[verify] baseline saved: $Baseline ($($snap.Count) releases)"
} elseif (Test-Path $baselinePath) {
    $base = Get-Content $baselinePath -Raw | ConvertFrom-Json
    $oldCount = 0
    $oldOk = 0
    foreach ($b in $base) {
        if ($b.tag -eq $tag) { continue }
        $oldCount++
        $now = $all | Where-Object { $_.tag_name -eq $b.tag }
        if (-not $now) { Check "old release $($b.tag) still exists" $false; continue }
        $same = $true
        foreach ($ba in $b.assets) {
            $na = $now.assets | Where-Object { $_.name -eq $ba.name }
            if (-not $na -or [int64]$na.size -ne [int64]$ba.size -or $na.digest -ne $ba.digest) { $same = $false }
        }
        if ($same) { $oldOk++ } else { Check "old release $($b.tag) unchanged" $false }
    }
    if ($oldCount -gt 0) {
        Check "all old releases unchanged" ($oldOk -eq $oldCount) "$oldOk/$oldCount"
    }
} else {
    Write-Host "[verify] no baseline file ($Baseline) - run with -SaveBaseline BEFORE publishing to get old-release diffs"
}

# 5) git side
if ($prePublish) {
    Write-Host "[skip] git sync checks (pre-publish snapshot; they are verified after the push)"
} else {
$head = (& git.exe -C $root rev-parse HEAD).Trim()
$remote = (& git.exe -C $root rev-parse origin/main 2>$null).Trim()
$lr = (& git.exe -C $root rev-list --left-right --count origin/main...HEAD 2>$null)
$clean = (@(& git.exe -C $root status --porcelain) -join "").Trim().Length -eq 0
Check "HEAD == origin/main" ($head -eq $remote) $head.Substring(0, 8)
if ($lr) { Check "no divergence (origin/main...HEAD)" ($lr -replace "\s+", "`t" -eq "0`t0") $lr }
Check "working tree clean" $clean
$tagOnRemote = (& git.exe -C $root ls-remote --tags origin "refs/tags/$tag" 2>$null)
Check "tag $tag exists on the remote" ([bool]$tagOnRemote)
}

Write-Host ""
Write-Host ("=== RESULT: {0} ===  checks={1} ok={2} fail={3}" -f `
    $(if ($script:failed -eq 0) { "PASS" } else { "FAIL" }), $script:checks, ($script:checks - $script:failed), $script:failed)
if ($script:failed -ne 0) { exit 1 }
