# fetch_scrcpy.ps1
#
# Downloads the official scrcpy Windows 64-bit release (which also bundles
# adb.exe and all required DLLs) and extracts it into the target directory.
#
# Usage:
#   ./scripts/fetch_scrcpy.ps1 -Version 4.1   -OutDir build/scrcpy
#   ./scripts/fetch_scrcpy.ps1 -Version latest -OutDir build/scrcpy
#
param(
    [string]$Version = "latest",
    [string]$OutDir = "build/scrcpy"
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# Resolve "latest" to the current release tag.
if ($Version -eq "latest") {
    Write-Host "Querying latest scrcpy version from GitHub..."
    # Authenticate when a token is available. Unauthenticated GitHub API requests
    # are limited to 60/hour *per IP*, and GitHub-hosted runners share IPs, so the
    # release build failed with "API rate limit exceeded" until the workflow
    # started passing GITHUB_TOKEN through. The token needs no scopes for a public
    # repository.
    $headers = @{ "User-Agent" = "AdbTools-build" }
    if ($env:GITHUB_TOKEN) {
        $headers["Authorization"] = "Bearer $env:GITHUB_TOKEN"
    } else {
        Write-Host "  (no GITHUB_TOKEN set - the API call is unauthenticated and may be rate limited)"
    }
    $release = Invoke-RestMethod -Uri "https://api.github.com/repos/Genymobile/scrcpy/releases/latest" -Headers $headers
    $Version = ($release.tag_name).TrimStart('v')
    if (-not $Version) { throw "Failed to resolve the latest scrcpy version." }
}
Write-Host "Using scrcpy v$Version"

$zipName = "scrcpy-win64-v$Version.zip"
$url     = "https://github.com/Genymobile/scrcpy/releases/download/v$Version/$zipName"
$work    = Join-Path $env:TEMP "scrcpy-fetch-$Version"
$zipPath = Join-Path $work $zipName

New-Item -ItemType Directory -Force -Path $work   | Out-Null
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Write-Host "Downloading $url ..."
Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing

Write-Host "Extracting ..."
Expand-Archive -Path $zipPath -DestinationPath $work -Force

# The zip contains a top-level folder like "scrcpy-win64-v4.1/". Copy its
# contents (scrcpy.exe, adb.exe, scrcpy-server, *.dll, LICENSE.txt, ...) into
# OutDir so the app finds them at <exe dir>/scrcpy/.
$inner = Get-ChildItem -Path $work -Directory |
         Where-Object { $_.Name -like "scrcpy-*" } |
         Select-Object -First 1
$src = if ($inner) { $inner.FullName } else { $work }

Copy-Item -Path (Join-Path $src '*') -Destination $OutDir -Recurse -Force

Remove-Item -Path $work -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Done. scrcpy v$Version (incl. adb.exe) is in: $OutDir"
