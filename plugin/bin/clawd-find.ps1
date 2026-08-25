<#
  clawd-find.ps1 — locate Clawdmeter units on the local network.

  Used by /clawd:setup only. The hook path never needs this: once paired, the
  address is in ~/.clawd/config. Prints one "<ip> <host>" line per unit found.

  Windows PowerShell 5.1: async TCP connects in batches of 64 keep a full /24
  sweep near two seconds without needing PS 7's -Parallel.
#>
param([string]$Prefix)

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
foreach ($u in (Get-Units -NetPrefix $Prefix)) { Write-Output ("$($u.Ip) $($u.Host)") }
