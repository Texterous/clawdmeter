<#
  clawd-push.ps1 — Claude Code statusLine hook that feeds a Clawdmeter display.

  Claude Code hands us the session JSON on stdin, including rate_limits for
  Claude.ai subscribers. We map those four numbers onto the device's /api/usage
  contract, POST them to the unit on the LAN, and print a status line.

  Two rules govern everything here:
    1. Never block. This runs on every status line render. The POST is capped at
       one second and rediscovery runs in a detached process, because a stalled
       status line is worse than a stale panel.
    2. Never fail loudly. A missing device, an absent rate_limits block, or a
       chained command that dies must still leave a usable status line behind.

  Written for Windows PowerShell 5.1, which is the one every Windows box has —
  so no ternaries, no null-coalescing, and no ForEach-Object -Parallel.

  Config lives in ~/.clawd/config as key=value lines, written by /clawd:setup.
  Modes:
    (no args)          read stdin, push, print a status line
    -Scan [prefix]     sweep a /24 and print "<ip> <host>" for every unit found
    -Discover          re-resolve the configured unit and update the config
#>
param(
  [switch]$Discover,
  [switch]$Scan,
  [string]$Prefix
)

$ErrorActionPreference = 'SilentlyContinue'
$ProgressPreference    = 'SilentlyContinue'   # PS 5.1: the progress bar makes Invoke-* crawl

$Dir = Join-Path $HOME '.clawd'
$Cfg = Join-Path $Dir 'config'

function Read-Config {
  $h = @{}
  if (Test-Path $Cfg) {
    foreach ($line in (Get-Content $Cfg)) {
      # Strip a leading BOM: anything that wrote this file with PS 5.1's
      # -Encoding UTF8 left one, and it would silently rename the first key.
      $line = $line.TrimStart([char]0xFEFF)
      $i = $line.IndexOf('=')
      if ($i -gt 0) { $h[$line.Substring(0, $i)] = $line.Substring($i + 1) }
    }
  }
  return $h
}

function Write-ConfigValue {
  param([string]$Key, [string]$Value)
  $h = Read-Config
  $h[$Key] = $Value
  $lines = @()
  foreach ($k in $h.Keys) { $lines += ($k + '=' + $h[$k]) }
  # UTF8 without a BOM, explicitly. Set-Content -Encoding UTF8 means "with BOM"
  # on PS 5.1 and "without" on PS 7, and the pusher has to read back whatever
  # the other edition wrote.
  [System.IO.File]::WriteAllLines($Cfg, $lines, (New-Object System.Text.UTF8Encoding($false)))
}

# ---- discovery -------------------------------------------------------------
# No proxy, ever. A unit on the LAN never goes through one, and WPAD
# autodetection cost 350 ms on the first call in a cold powershell.exe — and
# this script starts cold on every status line render. Measured 352 ms -> 2 ms.
try { [System.Net.WebRequest]::DefaultWebProxy = $null } catch {}

# The /24 this machine is on. The UDP-connect trick needs no route parsing, no
# interface guessing, and no NetTCPIP module, and it sends nothing — Connect on
# a datagram socket only fixes the local endpoint the kernel would use.
function Get-LocalPrefix {
  try {
    $s = New-Object System.Net.Sockets.Socket('InterNetwork', 'Dgram', 'Udp')
    $s.Connect('1.1.1.1', 65530)
    $addr = $s.LocalEndPoint.Address.ToString()
    $s.Close()
    if ($addr -and $addr -ne '0.0.0.0') { return (($addr -split '\.')[0..2] -join '.') }
  } catch {}
  return $null
}

# Every Clawdmeter on a /24, as "<ip> <host>". Async TCP connects in batches of
# 64 keep a full sweep near two seconds without needing PS 7's -Parallel.
function Get-Units {
  param([string]$NetPrefix)
  if (-not $NetPrefix) { $NetPrefix = Get-LocalPrefix }
  if (-not $NetPrefix) { return @() }
  $found = @()
  for ($base = 1; $base -le 254; $base += 64) {
    $pending = @()
    for ($i = $base; ($i -lt $base + 64) -and ($i -le 254); $i++) {
      $addr = "$NetPrefix.$i"
      try {
        $c = New-Object System.Net.Sockets.TcpClient
        $pending += [pscustomobject]@{ Addr = $addr; Client = $c; Async = $c.BeginConnect($addr, 80, $null, $null) }
      } catch {}
    }
    Start-Sleep -Milliseconds 400
    foreach ($p in $pending) {
      $open = $false
      try { if ($p.Async.IsCompleted) { $p.Client.EndConnect($p.Async); $open = $true } } catch {}
      try { $p.Client.Close() } catch {}
      if ($open) {
        try {
          $r = Invoke-RestMethod -Uri "http://$($p.Addr)/api/status" -TimeoutSec 2 -ErrorAction Stop
          if ($r.fw -eq 'clawdmeter') { $found += [pscustomobject]@{ Ip = $p.Addr; Host = $r.host } }
        } catch {}
      }
    }
  }
  return $found
}

# mDNS first (nearly free when the resolver cooperates), then the /24 the unit
# was last seen on, then this machine's own /24 in case both of them moved.
function Find-Unit {
  param([string]$UnitHost)
  try {
    $r = Invoke-RestMethod -Uri "http://$UnitHost.local/api/status" -TimeoutSec 2 -ErrorAction Stop
    if ($r.host -eq $UnitHost) { return "$UnitHost.local" }
  } catch {}
  $cfg = Read-Config
  $prefixes = @()
  if ($cfg['ip']) { $prefixes += (($cfg['ip'] -split '\.')[0..2] -join '.') }
  $mine = Get-LocalPrefix
  if ($mine -and ($prefixes -notcontains $mine)) { $prefixes += $mine }
  foreach ($p in $prefixes) {
    foreach ($u in (Get-Units -NetPrefix $p)) {
      if ($u.Host -eq $UnitHost) { return $u.Ip }
    }
  }
  return $null
}

if ($Scan) {
  foreach ($u in (Get-Units -NetPrefix $Prefix)) { Write-Output ("$($u.Ip) $($u.Host)") }
  exit 0
}

if ($Discover) {
  $cfg = Read-Config
  if ($cfg['host']) {
    $found = Find-Unit -UnitHost $cfg['host']
    if ($found) { Write-ConfigValue -Key 'ip' -Value $found }
  }
  exit 0
}

# ---- read the session JSON -------------------------------------------------
$Raw = [Console]::In.ReadToEnd()
$cfg = Read-Config
$ip    = $cfg['ip']
$unit  = $cfg['host']
$chain = $cfg['chain']

$s = $null; $w = $null; $sr = 0; $wr = 0

# Minutes until a reset, from an epoch-seconds value.
#
# [long] throughout, not [int]: epoch seconds pass Int32.MaxValue in 2038, and a
# cast overflow here is a TERMINATING error. Inside one shared try block that
# aborted the whole parse, so a good five_hour reading silently took seven_day
# down with it. Each window now also gets its own try for the same reason.
function Get-ResetMinutes {
  param($ResetsAt, [long]$Now)
  if (-not $ResetsAt) { return 0 }
  $m = [math]::Floor(([long]$ResetsAt - $Now) / 60)
  if ($m -lt 0) { return 0 }
  return $m
}

try {
  $j = $Raw | ConvertFrom-Json
  $rl = $j.rate_limits
  if ($rl) {
    # Locale-safe epoch: -UFormat %s and [double]::Parse both depend on culture,
    # and this box is nl-NL where "." is not the decimal separator.
    $now = [long]((Get-Date).ToUniversalTime() - (Get-Date "1970-01-01T00:00:00Z").ToUniversalTime()).TotalSeconds
    if ($rl.five_hour) {
      try {
        $s  = [int]$rl.five_hour.used_percentage
        $sr = Get-ResetMinutes -ResetsAt $rl.five_hour.resets_at -Now $now
      } catch {}
    }
    if ($rl.seven_day) {
      try {
        $w  = [int]$rl.seven_day.used_percentage
        $wr = Get-ResetMinutes -ResetsAt $rl.seven_day.resets_at -Now $now
      } catch {}
    }
  }
} catch {}

# ---- push ------------------------------------------------------------------
# The device keeps its last good values on ok:false and flags the error, so an
# absent rate_limits block is reported honestly rather than sent as zeroes.
$pushed = $false
if ($ip) {
  if ($null -ne $s) {
    if     ($s -ge 100) { $st = 'rejected' }
    elseif ($s -ge 80)  { $st = 'allowed_warning' }
    else                { $st = 'allowed' }
    $wv = 0
    if ($null -ne $w) { $wv = $w }
    $body = '{"s":' + $s + ',"sr":' + $sr + ',"w":' + $wv + ',"wr":' + $wr + ',"st":"' + $st + '","ok":true}'
  } else {
    $body = '{"ok":false}'
  }
  try {
    Invoke-RestMethod -Uri "http://$ip/api/usage" -Method Post -Body $body `
      -ContentType 'application/json' -TimeoutSec 1 -ErrorAction Stop | Out-Null
    $pushed = $true
  } catch {}
}

# ---- rediscover on failure, detached, at most once a minute -----------------
# A DHCP lease change or an AP reboot moves the unit. Spawning a hidden process
# means the sweep cost lands on the NEXT render, not this one.
if ((-not $pushed) -and $ip -and $unit) {
  $stamp = Join-Path $Dir 'rediscover.stamp'
  $due = $true
  if (Test-Path $stamp) {
    $age = (Get-Date) - (Get-Item $stamp).LastWriteTime
    if ($age.TotalSeconds -lt 60) { $due = $false }
  }
  if ($due) {
    Set-Content -Path $stamp -Value 'x'
    try {
      Start-Process -FilePath 'powershell.exe' -WindowStyle Hidden `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                        $PSCommandPath, '-Discover') | Out-Null
    } catch {}
  }
}

# ---- status line -----------------------------------------------------------
# A chained command owns the line if the user already had one; we only ever
# append our own segment. Ours is deliberately terse: the panel is the display.
if ($null -ne $s) {
  $wv = 0
  if ($null -ne $w) { $wv = $w }
  $seg = "clawd 5h $s% | 7d $wv%"
} else {
  $seg = 'clawd waiting'
}
if (-not $pushed) { $seg = "$seg (offline)" }

if ($chain) {
  try {
    $out = $Raw | powershell.exe -NoProfile -Command $chain 2>$null
    if ($out) {
      Write-Output ((($out -join "`n").TrimEnd()) + '  ' + $seg)
      exit 0
    }
  } catch {}
}
Write-Output $seg
