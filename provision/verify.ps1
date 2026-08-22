# Pre-event checklist: walk every provisioned unit and print one table.
#
#   .\verify.ps1 -Units 1..30
#   .\verify.ps1 -Hosts clawdmeter-01,clawdmeter-07
#
# Resolves each unit by its mDNS name, which is the durable handle — DHCP moves
# addresses, hostnames do not. A unit that does not answer is listed as DOWN
# rather than silently skipped: a short table with rows missing reads as "all
# fine" when it is not.

[CmdletBinding()]
param(
  [int[]]$Units,
  [string[]]$Hosts,
  [int]$TimeoutSec = 4
)

$ErrorActionPreference = 'Continue'

if (-not $Hosts) {
  if (-not $Units) { $Units = 1..10 }
  $Hosts = $Units | ForEach-Object { 'clawdmeter-{0:d2}' -f $_ }
}

$rows = foreach ($h in $Hosts) {
  $r = [ordered]@{ Unit = $h; State = 'DOWN'; Version = ''; IP = ''; RSSI = ''
                   Heap = ''; Uptime = ''; Meter = ''; Age = '' }
  try {
    $s = Invoke-RestMethod "http://$h.local/api/status" -TimeoutSec $TimeoutSec
    $r.State   = if ($s.mode -eq 'ap') { 'SETUP-AP' } else { 'ok' }
    $r.Version = $s.version
    $r.IP      = $s.ip
    $r.RSSI    = $s.rssi
    $r.Heap    = $s.heap
    $r.Uptime  = [math]::Round($s.uptime / 60)
    if ($s.meter.valid) {
      $r.Meter = '{0}% / {1}%' -f [int]$s.meter.sessionPct, [int]$s.meter.weeklyPct
      $r.Age   = '{0}s' -f [int]$s.meter.ageSec
    } else {
      $r.Meter = 'no data'
    }
  } catch { }
  [pscustomobject]$r
}

$rows | Format-Table -AutoSize

$down  = @($rows | Where-Object State -eq 'DOWN')
$setup = @($rows | Where-Object State -eq 'SETUP-AP')
$nodata = @($rows | Where-Object { $_.State -eq 'ok' -and $_.Meter -eq 'no data' })

Write-Host ''
Write-Host ("  {0}/{1} answering" -f ($rows.Count - $down.Count), $rows.Count)
if ($down.Count)  { Write-Host ("  {0} DOWN: {1}" -f $down.Count, ($down.Unit -join ', ')) -ForegroundColor Red }
if ($setup.Count) { Write-Host ("  {0} sitting on the setup hotspot (never joined WiFi): {1}" -f $setup.Count, ($setup.Unit -join ', ')) -ForegroundColor Yellow }
if ($nodata.Count) {
  Write-Host ("  {0} up but never pushed to: {1}" -f $nodata.Count, ($nodata.Unit -join ', ')) -ForegroundColor Yellow
  Write-Host '  (expected before anyone installs the agent - not a fault)' -ForegroundColor DarkGray
}
if (-not $down.Count -and -not $setup.Count) { Write-Host '  All units reachable.' -ForegroundColor Green }
