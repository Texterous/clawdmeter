# Pre-event checklist: walk every unit you can reach and print one table.
#
#   .\verify.ps1 -Units 1..30                      # clawd-01 .. clawd-30 by name
#   .\verify.ps1 -Hosts clawd-01,clawd-07
#   .\verify.ps1 -Hosts 192.168.1.57,192.168.1.58  # by address
#
# This sees a unit only while it is on the same network as this machine.
# startMdns() runs on station-mode success only (firmware/src/Net.cpp), so a unit
# sitting on its own setup hotspot has no .local name at all and is on a different
# network besides — it cannot answer here by either handle. That is why a unit
# that does not answer is reported as UNREACH rather than DOWN: the two states
# this script cannot tell apart are "dead" and "waiting on its own hotspot for
# someone to set it up", and calling that DOWN sent people looking for a fault
# that was not there.
#
# A giveaway batch is therefore not verifiable from here at all: every unit is
# supposed to be on its own hotspot until its recipient commissions it. Check
# those at the bench, off the panel — it shows SET ME UP and the unit's own
# hotspot name.
#
# Accepts addresses as well as names because of the same asymmetry: by-address is
# the only way to reach the losing half of a duplicate-hostname pair, which is
# exactly the fault the summary below hunts for.

[CmdletBinding()]
param(
  [int[]]$Units,
  [string[]]$Hosts,
  # Matches DEFAULT_HOSTNAME in firmware/src/config.h and the name flash.ps1
  # imports. A batch flashed before the rename answers to -Prefix clawdmeter.
  [string]$Prefix = 'clawd',
  [int]$TimeoutSec = 4
)

$ErrorActionPreference = 'Continue'

if (-not $Hosts) {
  if (-not $Units) { $Units = 1..10 }
  $Hosts = $Units | ForEach-Object { "$Prefix-{0:d2}" -f $_ }
}

# A bare unit name is an mDNS label and needs .local; anything that already has a
# dot or a colon resolves on its own. Gluing .local onto an address resolves to
# nothing, which is what made checking a unit by IP impossible.
function targetHost([string]$h) {
  if ($h -match '[.:]') { return $h }
  return "$h.local"
}

$rows = foreach ($h in $Hosts) {
  $r = [ordered]@{ Unit = $h; State = 'UNREACH'; Reported = ''; Version = ''; IP = ''
                   RSSI = ''; Heap = ''; Uptime = ''; MDNS = ''; Meter = ''; Age = '' }
  try {
    $s = Invoke-RestMethod "http://$(targetHost $h)/api/status" -TimeoutSec $TimeoutSec
    $r.State    = if ($s.mode -eq 'ap') { 'SETUP-AP' } else { 'ok' }
    $r.Reported = $s.host
    $r.Version  = $s.version
    $r.IP       = $s.ip
    $r.RSSI     = $s.rssi
    $r.Heap     = $s.heap
    $r.Uptime   = [math]::Round($s.uptime / 60)
    # The firmware reports whether its mDNS probe came back with the name already
    # claimed. Older builds omit the key, so a blank column means "not reported",
    # not "fine".
    $r.MDNS     = if ($s.mdns) { $s.mdns } else { '' }
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

$live    = @($rows | Where-Object State -ne 'UNREACH')
$unreach = @($rows | Where-Object State -eq 'UNREACH')
$setup   = @($rows | Where-Object State -eq 'SETUP-AP')
$nodata  = @($rows | Where-Object { $_.State -eq 'ok' -and $_.Meter -eq 'no data' })

# ---------------------------------------------------------------------------
# Duplicate hostnames.
#
# Two units with the same hostname is silent by construction: MDNS.begin() is
# called with no host-probe callback, so the loser of the probe keeps the name in
# its own settings, never announces it, and every push aimed at that name lands on
# the winner instead. The recipient of the loser sees a blank screen forever and
# has nothing to look at that would say why. Three signals, because no single one
# catches every shape of it:
# ---------------------------------------------------------------------------
$dupWarn = @()

# (1) The unit that answered is not the unit we asked for. Only meaningful for
#     name targets — an address is not a claim about identity.
foreach ($r in $live) {
  if ($r.Unit -match '[.:]') { continue }
  if ($r.Reported -and $r.Reported -ne $r.Unit) {
    $dupWarn += "  $($r.Unit) resolved to a unit calling itself '$($r.Reported)' at $($r.IP)"
  }
}

# (2) Two names, one address: one unit is answering for both, so the other unit
#     of the pair is unreachable by name and its pushes are going to this one.
foreach ($g in ($live | Where-Object IP | Group-Object IP | Where-Object Count -gt 1)) {
  $dupWarn += "  $($g.Name) answers for $(($g.Group.Unit -join ', ')) — one unit, several names"
}

# (3) Two addresses reporting one hostname: the genuine duplicate. Only visible
#     when both were reached by address, which is why -Hosts takes addresses.
foreach ($g in ($live | Where-Object Reported | Group-Object Reported | Where-Object Count -gt 1)) {
  $ips = @($g.Group.IP | Where-Object { $_ } | Sort-Object -Unique)
  if ($ips.Count -gt 1) {
    $dupWarn += "  hostname '$($g.Name)' claimed by $($ips -join ' and ') — two units, one name"
  }
}

$taken = @($live | Where-Object MDNS -eq 'taken')

Write-Host ''
Write-Host ("  {0}/{1} answering" -f $live.Count, $rows.Count)

if ($unreach.Count) {
  Write-Host ("  {0} UNREACH: {1}" -f $unreach.Count, ($unreach.Unit -join ', ')) -ForegroundColor Red
  Write-Host '  UNREACH means one of two things and this script cannot tell them apart:' -ForegroundColor DarkGray
  Write-Host '  the unit is dead, or it is sitting on its own setup hotspot and is not on' -ForegroundColor DarkGray
  Write-Host '  this network to be asked. Look at the panel: SET ME UP means the second.' -ForegroundColor DarkGray
}
if ($setup.Count) {
  Write-Host ("  {0} reached while in setup mode: {1}" -f $setup.Count, ($setup.Unit -join ', ')) -ForegroundColor Yellow
  Write-Host '  (only possible when this machine is joined to that hotspot)' -ForegroundColor DarkGray
}
if ($dupWarn.Count) {
  Write-Host ("  {0} duplicate-name signal(s):" -f $dupWarn.Count) -ForegroundColor Red
  $dupWarn | ForEach-Object { Write-Host $_ -ForegroundColor Red }
  Write-Host '  Give one of the pair a different hostname before it goes out. Pushes to a' -ForegroundColor DarkGray
  Write-Host '  duplicated name all land on whichever unit won the mDNS probe.' -ForegroundColor DarkGray
}
if ($taken.Count) {
  Write-Host ("  {0} report their own name as already taken: {1}" -f $taken.Count, ($taken.Unit -join ', ')) -ForegroundColor Red
}
if ($nodata.Count) {
  Write-Host ("  {0} up but never pushed to: {1}" -f $nodata.Count, ($nodata.Unit -join ', ')) -ForegroundColor Yellow
  Write-Host '  (expected until someone runs the sender at it — not a fault)' -ForegroundColor DarkGray
}
if (-not $unreach.Count -and -not $setup.Count -and -not $dupWarn.Count -and -not $taken.Count) {
  Write-Host '  All units reachable, all names unique.' -ForegroundColor Green
}
