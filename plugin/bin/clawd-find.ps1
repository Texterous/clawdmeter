<#
  clawd-find.ps1 — locate Clawdmeter units on the local network.

  Two jobs, and the second is the one that keeps a pairing alive:

    (no args) / -Prefix   list every unit on a /24, as "<ip> <host>" lines.
                          Used by /clawd:setup to pick a unit.
    -Resolve <host>       print the current IP of ONE named unit, or nothing.
                          Used by clawd-report when the stored address stops
                          answering, which is what a DHCP renewal looks like from
                          here. Without this a paired unit went silent for good
                          and the only cure was re-running setup by hand.

  Windows PowerShell 5.1: async TCP connects in batches of 64 keep a full /24
  sweep near two seconds without needing PS 7's -Parallel.
#>
param([string]$Prefix, [string]$Resolve)

$ErrorActionPreference = 'SilentlyContinue'
$ProgressPreference    = 'SilentlyContinue'
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

# One named unit's current IP.
#
# Sweep FIRST, mDNS second, and the order is load-bearing rather than a
# preference. On Windows PowerShell 5.1 a failed Invoke-RestMethod to an
# unresolvable .local name leaves the thread pool in a state where the async TCP
# connects below never report complete: measured on this box, a sweep that finds
# the unit on its own finds ZERO open ports when it runs after that lookup. So the
# reliable path goes first, and .local — which is the flaky one here, and only
# earns its keep for a unit outside this /24 — runs only when the sweep came up
# empty and there is nothing left to poison.
#
# Note what the mDNS branch returns: /api/status reports the device's own numeric
# address, so a working .local lookup yields an IP with no DNS API at all.
# A name lookup that actually gives up. Invoke-RestMethod's -TimeoutSec does NOT
# bound DNS resolution on 5.1 — it applies to the request, which has not started
# yet — so an unresolvable .local name cost 6.6 s measured here and pushed the
# whole worst case past the hook's budget. BeginGetHostAddresses is bounded by a
# wait handle we own.
function Resolve-Dns {
  param([string]$Name, [int]$Ms = 800)
  try {
    $ar = [System.Net.Dns]::BeginGetHostAddresses($Name, $null, $null)
    if (-not $ar.AsyncWaitHandle.WaitOne($Ms)) { return $null }
    foreach ($a in [System.Net.Dns]::EndGetHostAddresses($ar)) {
      if ($a.AddressFamily -eq 'InterNetwork') { return $a.ToString() }
    }
  } catch {}
  return $null
}

function Resolve-Unit {
  param([string]$UnitHost)
  if (-not $UnitHost) { return $null }
  foreach ($u in (Get-Units)) { if ($u.Host -eq $UnitHost) { return $u.Ip } }
  $mdns = Resolve-Dns "$UnitHost.local"
  if ($mdns) {
    try {
      $r = Invoke-RestMethod -Uri "http://$mdns/api/status" -TimeoutSec 2 -ErrorAction Stop
      if ($r.host -eq $UnitHost) { return $mdns }
    } catch {}
  }
  return $null
}

if ($Resolve) {
  $ip = Resolve-Unit -UnitHost $Resolve
  if ($ip) { Write-Output $ip }
  exit 0
}
foreach ($u in (Get-Units -NetPrefix $Prefix)) { Write-Output ("$($u.Ip) $($u.Host)") }
