# Batch provisioning for a Clawdmeter giveaway run.
#
# What this does NOT do is the one manual step: getting a factory-fresh unit onto
# the venue network. Stock firmware has no entry point except its own access
# point, so that is one AP join per unit, about a minute, unavoidable.
#
# Everything after that happens over the LAN with no further network changes:
#
#   .\flash.ps1 -Ip 192.168.1.57 -Unit 07
#
# It uploads the slim image, waits for the unit to rejoin (baked credentials, see
# -Credentials), imports that unit's settings, optionally installs the full image
# on top, and verifies the result. Budget 2-3 minutes hands-on per unit; run trays
# of eight on a powered hub and pipeline it.
#
# First, once per batch, write the credential header the images bake in:
#   .\flash.ps1 -Credentials -Ssid "HackathonWiFi" -Password "..."
# That writes firmware/src/provision_local.h, which is gitignored.

[CmdletBinding(DefaultParameterSetName = 'Flash')]
param(
  # --- credential mode ---
  [Parameter(ParameterSetName = 'Credentials', Mandatory)][switch]$Credentials,
  [Parameter(ParameterSetName = 'Credentials', Mandatory)][string]$Ssid,
  [Parameter(ParameterSetName = 'Credentials', Mandatory)][string]$Password,

  # --- flash mode ---
  [Parameter(ParameterSetName = 'Flash', Mandatory)][string]$Ip,
  [Parameter(ParameterSetName = 'Flash')][string]$Unit = '01',
  [Parameter(ParameterSetName = 'Flash')][string]$SlimImage = '../dist/clawdmeter-ultra-slim.bin',
  [Parameter(ParameterSetName = 'Flash')][string]$FullImage = '../dist/clawdmeter-ultra.bin',
  [Parameter(ParameterSetName = 'Flash')][switch]$SkipFull,
  [Parameter(ParameterSetName = 'Flash')][string]$Batch = './batch.json'
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function step($m) { Write-Host "  -> $m" -ForegroundColor Cyan }
function ok($m)   { Write-Host "  OK  $m" -ForegroundColor Green }
function bad($m)  { Write-Host "  !!  $m" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# Credential mode: write the gitignored header both images include.
# A header rather than -D flags because an SSID with a space cannot survive
# PlatformIO's flag splitting, and a credential in platformio.ini gets committed.
# ---------------------------------------------------------------------------
if ($Credentials) {
  $esc = { param($s) $s -replace '\\', '\\\\' -replace '"', '\"' }
  $header = @"
// LOCAL FILE -- gitignored, never committed. See .gitignore.
//
// Written by provision/flash.ps1 -Credentials. Both config.h and loader.cpp
// include this when it exists, so a freshly flashed unit rejoins the venue
// network by itself and nobody touches a setup screen on event day.
#pragma once

#define PROVISION_SSID "$(& $esc $Ssid)"
#define PROVISION_PASS "$(& $esc $Password)"

// The rollback/step-down loader uses the same network, so no step of a recovery
// ever needs an access point.
#define LOADER_SSID PROVISION_SSID
#define LOADER_PASS PROVISION_PASS
"@
  $dest = '../firmware/src/provision_local.h'
  Set-Content -Path $dest -Value $header -Encoding utf8 -NoNewline
  ok "wrote $dest for SSID '$Ssid'"
  Write-Host ''
  Write-Host '  Now build the batch images:' -ForegroundColor Yellow
  Write-Host '    cd ../firmware'
  Write-Host '    pio run -e ultra_slim -e ultra -e loader'
  Write-Host '    mkdir ../dist; cp .pio/build/ultra_slim/firmware.bin ../dist/clawdmeter-ultra-slim.bin'
  exit 0
}

# ---------------------------------------------------------------------------
# Flash mode
# ---------------------------------------------------------------------------
if (-not (Test-Path $SlimImage)) { bad "no slim image at $SlimImage"; exit 1 }

$slimSize = (Get-Item $SlimImage).Length
Write-Host ''
Write-Host "Unit $Unit at $Ip" -ForegroundColor White
step "uploading slim image ($([math]::Round($slimSize/1KB)) KB)"

# curl.exe, not Invoke-WebRequest: multipart upload of a large binary to a
# single-threaded embedded server is exactly where IWR's buffering bites.
$out = & curl.exe -s -m 240 -F "firmware=@$SlimImage;filename=firmware.bin" "http://$Ip/update" 2>&1
if ($LASTEXITCODE -ne 0) { bad "upload failed: $out"; exit 1 }
if ($out -notmatch 'OK') { bad "rejected: $out"; exit 1 }
ok 'written, rebooting'

# ---- wait for it to come back on its own -----------------------------------
# Baked credentials mean it rejoins the same network, so we poll the same IP.
# DHCP may hand out a different one; -Ip is the address to poll, and the unit's
# mDNS name is the durable handle afterwards.
step 'waiting for it to rejoin'
$status = $null
foreach ($try in 1..40) {
  Start-Sleep -Seconds 3
  try {
    $status = Invoke-RestMethod "http://$Ip/api/status" -TimeoutSec 5
    if ($status.fw -eq 'clawdmeter') { break }
  } catch { }
}
if (-not $status -or $status.fw -ne 'clawdmeter') {
  bad "did not come back at $Ip. If DHCP moved it, find it and rerun with the new -Ip."
  bad "If it is on its setup hotspot instead, the image was built without credentials."
  exit 1
}
ok "$($status.fw) $($status.version) on '$($status.ssid)'"

# ---- per-unit settings ------------------------------------------------------
# /api/import is a partial merge, so this cannot clobber the baked WiFi row.
$settings = @{ hostname = "clawdmeter-$Unit" }
if (Test-Path $Batch) {
  $b = Get-Content $Batch -Raw | ConvertFrom-Json
  foreach ($p in $b.PSObject.Properties) {
    if ($p.Name -eq 'units') { continue }
    $settings[$p.Name] = $p.Value
  }
  $per = $b.units.$Unit
  if ($per) { foreach ($p in $per.PSObject.Properties) { $settings[$p.Name] = $p.Value } }
}
step "importing settings (hostname clawdmeter-$Unit)"
$body = $settings | ConvertTo-Json -Depth 6 -Compress
try {
  Invoke-RestMethod "http://$Ip/api/import" -Method Post -ContentType 'application/json' `
                    -Body $body -TimeoutSec 15 | Out-Null
  ok 'settings applied, rebooting'
} catch { bad "import failed: $($_.Exception.Message)"; exit 1 }

Start-Sleep -Seconds 8

# ---- optional: full image on top -------------------------------------------
# Not subject to stock's OTA ceiling any more, so this is where HTTPS and
# self-update get added.
if (-not $SkipFull) {
  if (Test-Path $FullImage) {
    step "uploading full image ($([math]::Round((Get-Item $FullImage).Length/1KB)) KB)"
    $out = & curl.exe -s -m 240 -F "firmware=@$FullImage;filename=firmware.bin" "http://$Ip/update" 2>&1
    if ($out -match 'OK') { ok 'written, rebooting'; Start-Sleep -Seconds 12 }
    else { bad "full image rejected: $out"; bad 'the slim image is still installed and working' }
  } else {
    step "no full image at $FullImage, staying on slim"
  }
}

# ---- verify -----------------------------------------------------------------
step 'verifying'
try {
  $final = Invoke-RestMethod "http://$Ip/api/status" -TimeoutSec 8
  ok ("{0} {1} | host {2} | heap {3} | rssi {4}" -f `
      $final.fw, $final.version, $final.host, $final.heap, $final.rssi)
  Write-Host ''
  Write-Host "  Unit $Unit done. Sticker: http://$($final.host).local" -ForegroundColor Green
} catch {
  bad "final check failed: $($_.Exception.Message)"
  exit 1
}
