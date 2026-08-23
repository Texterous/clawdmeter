# Clawdmeter agent bootstrap (Windows / PowerShell).
#
# Served by the device itself at http://<device>/agent/install.ps1, because the
# machine that needs this may be joined to the device's setup access point with
# no route to the internet. In that case it says so and tells you what to do,
# rather than hanging on a download that cannot succeed.
#
#   irm http://clawdmeter-XXXX.local/agent/install.ps1 | iex
#
# Optional: -Device to skip discovery, -Version to pin a release.

[CmdletBinding()]
param(
  [string]$Device = $env:CLAWDMETER_DEVICE,
  [string]$Version = 'latest'
)

$ErrorActionPreference = 'Stop'
$repo    = 'Texterous/clawdmeter'
$asset   = 'clawdmeter-agent-windows-x64.exe'
$destDir = Join-Path $env:LOCALAPPDATA 'Clawdmeter'
$dest    = Join-Path $destDir 'clawdmeter-agent.exe'

function Say($m) { Write-Host "  $m" }

Write-Host ''
Write-Host 'Clawdmeter agent installer' -ForegroundColor Cyan
Write-Host ''

# --- Is there a route to the internet at all? ------------------------------
# The common failure is running this while still joined to a Clawdmeter-Setup AP,
# which has no uplink. Detect it up front and be explicit.
try {
  $null = Invoke-WebRequest -Uri 'https://api.github.com' -Method Head -TimeoutSec 8 -UseBasicParsing
} catch {
  Write-Host 'No route to github.com.' -ForegroundColor Yellow
  Say 'If you are joined to a Clawdmeter-Setup network, that is expected:'
  Say 'it has no internet uplink. Finish setting up WiFi on the device, rejoin'
  Say 'your normal network, then run this again.'
  Say ''
  Say "Or download it by hand: https://github.com/$repo/releases/latest"
  exit 1
}

# --- Resolve the release asset --------------------------------------------
$api = if ($Version -eq 'latest') {
  "https://api.github.com/repos/$repo/releases/latest"
} else {
  "https://api.github.com/repos/$repo/releases/tags/$Version"
}

try {
  $rel = Invoke-RestMethod -Uri $api -Headers @{ 'User-Agent' = 'clawdmeter-installer' } -TimeoutSec 20
} catch {
  Write-Host "Could not read the release list ($Version)." -ForegroundColor Red
  Say $_.Exception.Message
  exit 1
}

$url = ($rel.assets | Where-Object { $_.name -eq $asset } | Select-Object -First 1).browser_download_url
if (-not $url) {
  Write-Host "Release $($rel.tag_name) has no $asset." -ForegroundColor Red
  Say "Available: $(($rel.assets.name) -join ', ')"
  exit 1
}

# --- Install --------------------------------------------------------------
if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir | Out-Null }
Say "Downloading $($rel.tag_name)..."
Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
Say "Installed to $dest"

# Put it on PATH for future shells (user scope; no admin needed).
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($userPath -notlike "*$destDir*") {
  [Environment]::SetEnvironmentVariable('Path', "$userPath;$destDir", 'User')
  Say 'Added to your PATH (new terminals will find it).'
}

# --- Start it -------------------------------------------------------------
# No -Device given means the agent discovers clawdmeter-*.local over mDNS,
# which is the path we want people on: no IP to look up, nothing to re-enter
# when DHCP moves the device.
Write-Host ''
if ($Device) {
  Say "Starting against $Device"
  & $dest --device $Device
} else {
  Say 'Starting with mDNS discovery'
  & $dest
}
