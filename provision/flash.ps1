# Batch provisioning for a Clawdmeter run.
#
# There are two kinds of batch and they are not the same job:
#
#   -Giveaway   Units handed to strangers, with no venue network to pre-seed.
#               Every unit has to come up on its own setup hotspot and be
#               commissioned by whoever receives it, so there is nothing to name
#               and nothing to import: one upload per unit, and the unit's own
#               chip suffix is its identity. Its panel is its label.
#
#   (default)   A staffed batch on a network you control — demo tables, a shelf
#               of units you run yourself. The network credential is baked into
#               the image, so a freshly flashed unit rejoins by itself and
#               everything after the upload is scripted over the LAN.
#
# The one manual step is the same either way, and it is unavoidable: getting a
# factory-fresh unit onto the bench network. Stock firmware has no entry point
# except its own access point, so that is one AP join per unit, about a minute.
#
#   .\flash.ps1 -Giveaway -Ip 192.168.1.57
#   .\flash.ps1 -Ip 192.168.1.57 -Unit 07
#
# For a staffed batch, first write the credential header the images bake in:
#   .\flash.ps1 -Credentials -Ssid "HackathonWiFi" -Password "..."
# That writes firmware/src/provision_local.h, which is gitignored. Never do this
# for a giveaway batch — see the guard below for why.

[CmdletBinding(DefaultParameterSetName = 'Named')]
param(
  # --- credential mode ---
  [Parameter(ParameterSetName = 'Credentials', Mandatory)][switch]$Credentials,
  [Parameter(ParameterSetName = 'Credentials', Mandatory)][string]$Ssid,
  [Parameter(ParameterSetName = 'Credentials', Mandatory)][string]$Password,

  # --- giveaway mode: upload only, nothing named, nothing imported ---
  [Parameter(ParameterSetName = 'Giveaway', Mandatory)][switch]$Giveaway,

  # --- both flashing modes ---
  [Parameter(ParameterSetName = 'Named',    Mandatory)]
  [Parameter(ParameterSetName = 'Giveaway', Mandatory)]
  [string]$Ip,

  [Parameter(ParameterSetName = 'Named')]
  [Parameter(ParameterSetName = 'Giveaway')]
  [string]$SlimImage = '../dist/clawdmeter-ultra-slim.bin',

  # -Unit is mandatory rather than defaulted. It used to default to '01', so a
  # run that forgot the flag named every unit in the batch the same thing —
  # thirty units answering to one mDNS name, and each recipient's pushes landing
  # on whichever one won the probe race.
  #
  # Six characters, and the limit is pixel width rather than taste. The setup
  # screen renders the AP SSID through gfxFitSize at up to text size 3, the font
  # is 6x8 scaled by an integer, and the usable content width is 232 px:
  #   "Clawd-" + 6 chars = 12 chars = 12 * 6 * 3 = 216 px, fits at size 3.
  #   13 chars would be 234 px, which silently drops the one string a recipient
  #   has to match against thirty near-identical hotspots down to size 2.
  # The .local URL is the tighter case at size 2:
  #   "clawd-" + 6 + ".local" = 18 chars = 18 * 6 * 2 = 216 px, fits.
  [Parameter(ParameterSetName = 'Named', Mandatory)]
  [ValidatePattern('^[A-Za-z0-9-]{1,6}$')]
  [string]$Unit,

  [Parameter(ParameterSetName = 'Named')][string]$FullImage = '../dist/clawdmeter-ultra.bin',
  [Parameter(ParameterSetName = 'Named')][switch]$SkipFull,
  [Parameter(ParameterSetName = 'Named')][string]$Batch = './batch.json'
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

function step($m) { Write-Host "  -> $m" -ForegroundColor Cyan }
function ok($m)   { Write-Host "  OK  $m" -ForegroundColor Green }
function bad($m)  { Write-Host "  !!  $m" -ForegroundColor Red }
function warn($m) { Write-Host "  ??  $m" -ForegroundColor Yellow }

# The default names, kept in step with firmware/src/config.h DEFAULT_AP_SSID and
# DEFAULT_HOSTNAME. The firmware appends a 4-hex chip suffix on first boot; a
# staffed batch replaces that with a sequential -Unit so the units are countable.
$AP_PREFIX   = 'Clawd'
$HOST_PREFIX = 'clawd'

$PROVISION_HEADER = '../firmware/src/provision_local.h'
$CONFIG_H         = '../firmware/src/config.h'

# ---------------------------------------------------------------------------
# Does an image carry a baked WiFi credential?
#
# The check is by name rather than by heuristic. config.h's HAS_PROVISION block
# turns PROVISION_SSID into a string literal inside Settings::setDefaults, so
# whenever the header was present at build time the SSID sits in the image
# verbatim. Searching for it is the only way to tell a provisioned build from a
# giveaway build after the fact — the .bin carries no other marker, and
# "I remember which env I built" is not a safeguard for thirty units.
# ---------------------------------------------------------------------------
function provisionedSsid {
  if (-not (Test-Path $PROVISION_HEADER)) { return $null }
  $m = [regex]::Match((Get-Content $PROVISION_HEADER -Raw),
                      '#define\s+PROVISION_SSID\s+"([^"]*)"')
  if ($m.Success -and $m.Groups[1].Value) { return $m.Groups[1].Value }
  return $null
}

# ---------------------------------------------------------------------------
# Is this image the firmware in the tree, or a leftover in dist/?
#
# dist/ is gitignored, hand-populated and easy to forget, and $SlimImage defaults
# into it — so the script's happy path was to upload whatever was there last. That
# is not a cosmetic problem: a stale artefact ships the bugs the tree has already
# fixed, and one of them was /api/export handing a recipient's home WiFi password
# to any unauthenticated caller. Nothing downstream would notice, because
# /api/status reported the same firmware name either way.
#
# FW_VERSION is the marker. It is a plain string literal in the image, config.h is
# the single source of truth for it, and it is cheap to read from both ends.
# ---------------------------------------------------------------------------
function expectedVersion {
  if (-not (Test-Path $CONFIG_H)) { return $null }
  $m = [regex]::Match((Get-Content $CONFIG_H -Raw),
                      '#define\s+FW_VERSION\s+"([^"]*)"')
  if ($m.Success -and $m.Groups[1].Value) { return $m.Groups[1].Value }
  return $null
}

function imageContains([string]$path, [string]$needle) {
  if (-not $needle) { return $false }
  # Latin-1, not UTF-8: an arbitrary byte sequence decodes losslessly this way,
  # so a byte that is not valid UTF-8 cannot shift the match or swallow it.
  $enc = [System.Text.Encoding]::GetEncoding(28591)
  return $enc.GetString([System.IO.File]::ReadAllBytes($path)).Contains($needle)
}

# Set-Location moves PowerShell's location, not the process working directory, so
# a relative path resolves differently for a .NET call or for curl.exe than it
# does for Test-Path. Absolute paths from here on: a mismatch shows up as "cannot
# open file" from curl after the guard has already passed on a file it did read.
function absPath([string]$path) { return (Convert-Path -LiteralPath $path) }

# ---------------------------------------------------------------------------
# Credential mode: write the gitignored header both images include.
# A header rather than -D flags because an SSID with a space cannot survive
# PlatformIO's flag splitting, and a credential in platformio.ini gets committed.
# ---------------------------------------------------------------------------
if ($Credentials) {
  Write-Host ''
  warn 'This bakes one network into every image built from now on.'
  warn 'A giveaway batch must NOT carry it: thirty units seeded with a network'
  warn 'that is not at the venue all fail to join, and none of them fall back to'
  warn 'anything a recipient can use except the setup hotspot they should have'
  warn 'started on. For a giveaway batch, build with provisioning compiled out'
  warn '(-e ultra_giveaway, or with this header absent) and flash with -Giveaway.'
  Write-Host ''

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
  Set-Content -Path $PROVISION_HEADER -Value $header -Encoding utf8 -NoNewline
  ok "wrote $PROVISION_HEADER for SSID '$Ssid'"
  Write-Host ''
  Write-Host '  Now build the batch images:' -ForegroundColor Yellow
  Write-Host '    cd ../firmware'
  Write-Host '    pio run -e ultra_slim -e ultra -e loader'
  Write-Host '    mkdir ../dist; cp .pio/build/ultra_slim/firmware.bin ../dist/clawdmeter-ultra-slim.bin'
  exit 0
}

# ---------------------------------------------------------------------------
# Pick the image, and check it is the image this mode is allowed to upload.
# ---------------------------------------------------------------------------
# A giveaway build is a different artefact, so it gets its own filename when one
# has been produced. Only substituted when the caller did not name an image.
if ($Giveaway -and -not $PSBoundParameters.ContainsKey('SlimImage')) {
  $gv = '../dist/clawdmeter-ultra-giveaway.bin'
  if (Test-Path $gv) { $SlimImage = $gv }
}

if (-not (Test-Path $SlimImage)) { bad "no image at $SlimImage"; exit 1 }
$SlimImage = absPath $SlimImage

# Version guard, before the credential guard: an image from the wrong build is
# wrong for both batch types, and refusing is the only safe answer when the whole
# point of the run is that nobody can reach these units afterwards.
$want = expectedVersion
if ($want) {
  if (imageContains $SlimImage $want) {
    ok "image is firmware $want (matches $CONFIG_H)"
  } else {
    bad "$SlimImage is not built from this tree."
    bad "config.h says FW_VERSION $want; that string is not in the image."
    bad 'Almost always this means dist/ is stale. Rebuild and refresh it:'
    bad '    cd ../firmware'
    if ($Giveaway) {
      bad '    pio run -e ultra_giveaway'
      bad '    cp .pio/build/ultra_giveaway/firmware.bin ../dist/clawdmeter-ultra-giveaway.bin'
    } else {
      bad '    pio run -e ultra_slim -e ultra'
      bad '    cp .pio/build/ultra_slim/firmware.bin ../dist/clawdmeter-ultra-slim.bin'
      bad '    cp .pio/build/ultra/firmware.bin      ../dist/clawdmeter-ultra.bin'
    }
    bad '  (or pass -SlimImage with the path to the image you mean)'
    exit 1
  }
} else {
  warn "could not read FW_VERSION from $CONFIG_H, so the image's age is unchecked."
}

$baked = provisionedSsid
if ($Giveaway) {
  # Refuse rather than warn. Getting this wrong is not recoverable at the venue:
  # the units are already in other people's hands, they are all trying to join a
  # network that is not there, and there is no paper telling anyone what to do.
  if ($baked -and (imageContains $SlimImage $baked)) {
    bad "$SlimImage was built with provisioning compiled in."
    bad "It carries the network '$baked', which would be baked into every unit"
    bad 'in this batch. Rebuild with provisioning off before flashing a giveaway:'
    bad '    cd ../firmware'
    bad '    pio run -e ultra_giveaway'
    bad "  (or move $PROVISION_HEADER aside and rebuild -e ultra_slim)"
    exit 1
  }
  if ($baked) {
    ok "image carries no baked network (checked for '$baked')"
  } else {
    # No header to check against, so the image cannot be cleared by name. Say so
    # instead of implying a check happened.
    warn "no $PROVISION_HEADER to check against, so this image could not be"
    warn 'verified by name. The post-upload check below is the real test: a'
    warn 'giveaway unit must stop answering on the LAN.'
  }
} elseif (-not $baked) {
  warn 'no provisioning header, so this image opens its own setup hotspot after'
  warn 'the upload instead of rejoining. The steps below all run over the LAN and'
  warn 'will fail. Use -Giveaway, or write the header with -Credentials first.'
}

$slimSize = (Get-Item $SlimImage).Length
Write-Host ''
if ($Giveaway) { Write-Host "Giveaway unit at $Ip" -ForegroundColor White }
else           { Write-Host "Unit $Unit at $Ip"    -ForegroundColor White }
step "uploading $(Split-Path -Leaf $SlimImage) ($([math]::Round($slimSize/1KB)) KB)"

# curl.exe, not Invoke-WebRequest: multipart upload of a large binary to a
# single-threaded embedded server is exactly where IWR's buffering bites.
$out = & curl.exe -s -m 240 -F "firmware=@$SlimImage;filename=firmware.bin" "http://$Ip/update" 2>&1
if ($LASTEXITCODE -ne 0) { bad "upload failed: $out"; exit 1 }
if ($out -notmatch 'OK') { bad "rejected: $out"; exit 1 }
ok 'written, rebooting'

# ---------------------------------------------------------------------------
# Giveaway mode ends here, and the check is that the unit goes QUIET.
#
# A factory-fresh LittleFS has no saved network, so a unit running an image with
# provisioning compiled out has nothing to join and opens its own hotspot. That
# puts it on a different network from this machine, which is the intended end
# state — so silence is the pass and an answer is the failure.
# ---------------------------------------------------------------------------
if ($Giveaway) {
  step 'confirming it does NOT rejoin the LAN'
  $late = $null
  foreach ($try in 1..15) {
    Start-Sleep -Seconds 3
    try { $late = Invoke-RestMethod "http://$Ip/api/status" -TimeoutSec 4 } catch { $late = $null }
    if ($late -and $late.fw -eq 'clawdmeter' -and $late.mode -ne 'ap') { break }
    $late = $null
  }
  if ($late) {
    bad "it rejoined '$($late.ssid)' as $($late.host)."
    bad 'That means the image had a network baked in after all, so this unit —'
    bad 'and every other unit flashed from this image — will not open a setup'
    bad 'hotspot for its recipient. Rebuild with -e ultra_giveaway and reflash.'
    exit 1
  }
  ok 'silent on the LAN, as a giveaway unit should be'
  Write-Host ''
  Write-Host '  Now read the panel before you box it:' -ForegroundColor Green
  Write-Host "    SET ME UP  /  Join this WiFi:  /  $AP_PREFIX-xxxx" -ForegroundColor Green
  Write-Host '  Write those four hex characters on the tally. Two units showing the' -ForegroundColor DarkGray
  Write-Host '  same suffix is the one collision nothing downstream can catch, and' -ForegroundColor DarkGray
  Write-Host '  this screen is the only place it is visible.' -ForegroundColor DarkGray
  exit 0
}

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
# Assert, not just print. The pre-upload guard checks the file; this checks what is
# actually running, which also catches an upload that silently kept the old sketch.
if ($want -and $status.version -ne $want) {
  bad "it reports $($status.version) but this tree is $want — the upload did not take."
  exit 1
}

# ---- per-unit settings ------------------------------------------------------
# /api/import is a partial merge, so this cannot clobber the baked WiFi row.
#
# apSsid goes with the hostname, not after it. Setting only the hostname left the
# unit answering to clawd-07 while broadcasting the chip-suffix hotspot name it
# was born with, so the name on the tally and the name in the air disagreed — and
# the one a person has to match is the one on the air. settingsApplyJson already
# accepts apSsid (Settings.cpp), so this needs nothing from the firmware.
$settings = @{
  hostname = "$HOST_PREFIX-$Unit"
  apSsid   = "$AP_PREFIX-$Unit"
}
if (Test-Path $Batch) {
  $b = Get-Content $Batch -Raw | ConvertFrom-Json
  foreach ($p in $b.PSObject.Properties) {
    # "units" is the per-unit table, and anything underscored is a note to the
    # reader. settingsApplyJson ignores keys it does not know, so sending them
    # was harmless — but it put the file's own prose in the request body.
    if ($p.Name -eq 'units' -or $p.Name.StartsWith('_')) { continue }
    $settings[$p.Name] = $p.Value
  }
  $per = $b.units.$Unit
  if ($per) { foreach ($p in $per.PSObject.Properties) { $settings[$p.Name] = $p.Value } }
}
step "importing settings (hostname $($settings.hostname), hotspot $($settings.apSsid))"
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
    $FullImage = absPath $FullImage
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
  Write-Host "  Unit $Unit done. Reach it at http://$($final.host).local" -ForegroundColor Green
} catch {
  bad "final check failed: $($_.Exception.Message)"
  exit 1
}
