<#
SutureOS release helper: publish whatever changed, with a version tag.

This encodes the release discipline the project follows:

  * every batch of changes is committed to main and pushed to
    https://github.com/Fanqi89/SutureOS,
  * every release gets an ANNOTATED tag named v<version> where <version> is the
    SemVer 2.0.0 string from include/sutureos/version.h (so code, tag, banner
    and ISO manifest always agree),
  * the tag message describes what that version does ("这个版本做什么了"),
  * existing tags are never moved or deleted, so old and new versions coexist.

Usage:
  # commit everything, tag the current version (needs a notes file) and push
  powershell -NoProfile -File scripts\release.ps1 -Message "feat: ..." `
      -NotesFile .\build\release-notes.md

  # just push the current state + tags (e.g. the tag already exists)
  powershell -NoProfile -File scripts\release.ps1 -PushOnly

GitHub is only reachable through the local HTTP proxy on this machine, so the
proxy configured in the Windows system settings is applied to every git call
(override with -Proxy).
#>
param(
    [string]$Message   = "",
    [string]$NotesFile = "",
    [string]$Proxy     = "",
    [switch]$PushOnly,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$repo = 'https://github.com/Fanqi89/SutureOS'

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

function Get-SystemProxy {
    if ($Proxy) { return $Proxy }
    $reg = Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings' -ErrorAction SilentlyContinue
    if ($reg -and $reg.ProxyEnable -eq 1 -and $reg.ProxyServer) {
        return "http://$($reg.ProxyServer)"
    }
    return ""
}

$version = Get-Version
$tag = "v$version"
Write-Host "[release] version: $version (tag $tag)"

Push-Location $root
try {
    $proxyArg = Get-SystemProxy
    if ($proxyArg) {
        Write-Host "[release] proxy: $proxyArg"
        $gitArgs = @('-c', "http.proxy=$proxyArg")
    } else {
        $gitArgs = @()
    }

    # NOTE: do NOT name this helper "Git" - PowerShell is case-insensitive, so a
    # call to `git` inside `function Git` re-enters the function itself and dies
    # with "call depth overflow". Use the real executable explicitly.
    function Invoke-Git([string[]]$argv) {
        $all = $gitArgs + $argv
        if ($DryRun) { Write-Host "[dry-run] git $($argv -join ' ')"; return 0 }
        & git.exe @all
        return $LASTEXITCODE
    }

    if (-not $PushOnly) {
        # 1) commit anything that changed
        $dirty = (& git.exe status --porcelain) -join "`n"
        if ($dirty.Trim().Length -gt 0) {
            if (-not $Message) { throw "the working tree has changes but -Message was not given" }
            Write-Host "[release] committing changes"
            if ((Invoke-Git @('add', '-A')) -ne 0) { throw "git add failed" }
            $msgFile = Join-Path $env:TEMP 'sutureos-release-commit.txt'
            [System.IO.File]::WriteAllText($msgFile, $Message, (New-Object System.Text.UTF8Encoding($false)))
            if ((Invoke-Git @('-c', 'i18n.commitEncoding=utf-8', 'commit', '-q', '-F', $msgFile)) -ne 0) {
                throw "git commit failed"
            }
        } else {
            Write-Host "[release] working tree clean, nothing to commit"
        }

        # 2) annotated tag for this version (never move an existing one)
        $exists = (& git.exe tag -l $tag)
        if ($exists) {
            Write-Host "[release] tag $tag already exists - leaving it untouched (old versions stay)"
        } else {
            if (-not $NotesFile) { throw "tag $tag does not exist yet: pass -NotesFile with what this version does" }
            if (-not (Test-Path $NotesFile)) { throw "notes file not found: $NotesFile" }
            Write-Host "[release] creating annotated tag $tag"
            if ((Invoke-Git @('-c', 'i18n.commitEncoding=utf-8', 'tag', '-a', $tag, '-F', $NotesFile)) -ne 0) {
                throw "git tag failed"
            }
        }
    }

    # 3) publish
    Write-Host "[release] pushing main + tags"
    if ((Invoke-Git @('push', 'origin', 'main')) -ne 0) { throw "git push main failed" }
    if ((Invoke-Git @('push', 'origin', '--tags')) -ne 0) {
        Write-Host "[release] note: --tags reported an error (an old tag may already exist remotely); checking state"
    }
    & git.exe -c "http.proxy=$proxyArg" ls-remote --heads --tags origin 2>$null | ForEach-Object { "  $_" }
    Write-Host "[release] done. repository: $repo/tree/main  tags: $repo/tags"
}
finally {
    Pop-Location
}
