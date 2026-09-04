<#
  clawd-agent.ps1 — the always-on sender behind a Clawdmeter display.

  Hooks were the whole sender before this, and hooks have one shape of hole in
  them: they only fire while Claude Code is open. Close the lid at six and the
  panel has nothing arriving all evening; power-cycle the device and nothing
  arrives until the next tool call, which on a shut laptop is never. Every
  community status-light project for Claude Code converged on the same answer —
  hooks are fire-and-forget clients, and one long-running process owns the device.
  This is that process.

  What it does, once a second:

    1. Reads the live sessions out of ~/.claude/sessions/*.json and derives what
       each one is doing from its transcript's mtime. No hooks needed, so no
       settings.json edit and no Claude Code restart during setup.
    2. Prefers a hook's state file when there is a fresh one, because a hook is
       TOLD what happened and this is inference. That is what keeps "blocked on a
       permission prompt" — the one state a transcript cannot show — on the glass.
    3. Pushes on change, plus a heartbeat every HB seconds. The heartbeat is
       declared in the payload (hb), which is what lets the device shorten its
       stale window from thirty minutes to a minute: it now knows how often
       silence should be broken.
    4. Listens on ONE tcp port for the device asking to be pushed to (GET
       /refresh). A rebooted unit asks the moment it is back, so a power cycle
       costs about a second of stale board instead of a heartbeat.
    5. Learns the device's address from that same request. Between this and the
       device learning ours from the source IP of every push, neither side has to
       sweep a subnet again after the first pairing — which matters here, because
       both ends of this link move: the device re-rolls its DHCP lease on most
       boots and the laptop roams between /24s on the same SSID.

  -Install registers it to start at logon and starts it now. It is deliberately
  a plain HKCU Run entry rather than a scheduled task: no elevation, no service
  account, and one registry value to remove.

  Everything here is best-effort. A missing device, a locked transcript, a
  refused port — each is logged and skipped. The panel keeps showing the last
  board it had, which is the entire point of the exercise.
#>
param(
  [switch]$Install,      # copy to ~/.clawd/bin, register at logon, start now
  [switch]$Uninstall,    # stop, unregister, remove the copy
  [switch]$Once,         # build a board, push it, log what happened, exit
  [switch]$Report,       # print state without touching anything
  [int]$Port = 0         # override the listener port (default CLAWD_AGENT_PORT)
)

$ErrorActionPreference = 'SilentlyContinue'
$ProgressPreference    = 'SilentlyContinue'
# 352 ms of WPAD autodetection on the first request out of a cold powershell.exe,
# measured on this box, against 2 ms with the proxy off. This process makes a
# request a second.
try { [System.Net.WebRequest]::DefaultWebProxy = $null } catch {}

# ---- paths and constants ---------------------------------------------------
$ClawdDir  = Join-Path $HOME '.clawd'
$ConfPath  = Join-Path $ClawdDir 'config'
$HookDir   = Join-Path $ClawdDir 'sessions'      # written by clawd-report (hooks)
$StatePath = Join-Path $ClawdDir 'agent.state'   # our own per-session state clock
$LogPath   = Join-Path $ClawdDir 'agent.log'
$PidPath   = Join-Path $ClawdDir 'agent.pid'
$BinDir    = Join-Path $ClawdDir 'bin'
$ClaudeDir = Join-Path $HOME '.claude'
$SessDir   = Join-Path $ClaudeDir 'sessions'
$ProjDir   = Join-Path $ClaudeDir 'projects'

$DEFAULT_PORT = 8788   # keep in step with CLAWD_AGENT_PORT in firmware/src/config.h
$HB           = 15     # s — heartbeat; the device trusts 3 of these plus grace
$TICK_MS      = 1000
$WORKING_SEC  = 25     # transcript touched this recently => the model holds the turn
$HOOK_TRUST   = 90     # s — a hook state file this fresh outranks our inference
$DEAD_SEC     = 900    # s — a transcript quiet this long is not a live session
$MAXROWS      = 6      # rows the 240x240 board renders; ns carries the true count
$NAMELEN      = 12     # what the panel renders at text size 2
$REFIND_SEC   = 60     # min gap between sweeps for a unit that stopped answering
$LOG_MAX      = 65536

if ($Port -le 0) { $Port = $DEFAULT_PORT }

New-Item -ItemType Directory -Force $ClawdDir | Out-Null

function Now-Epoch { [long][DateTimeOffset]::UtcNow.ToUnixTimeSeconds() }

# Via DateTimeOffset, never a DateTime subtraction: [datetime]'1970-01-01T00:00:00Z'
# is Kind=Local, so subtracting it silently adds the local UTC offset — an hour of
# error there once made every state file look expired the instant it was written.
function File-Epoch {
  param($File)
  if (-not $File) { return 0 }
  return [long]([DateTimeOffset]::new($File.LastWriteTimeUtc, [TimeSpan]::Zero).ToUnixTimeSeconds())
}

function Write-Log {
  param([string]$Msg)
  $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Msg
  try {
    if ((Test-Path $LogPath) -and ((Get-Item $LogPath).Length -gt $LOG_MAX)) {
      # Keep the tail, drop the head. A log that grows without bound on a desk
      # toy is a bug report waiting to happen.
      $keep = Get-Content $LogPath -Tail 200
      [System.IO.File]::WriteAllText($LogPath, (($keep -join "`n") + "`n"))
    }
    Add-Content -Path $LogPath -Value $line
  } catch {}
  if ($Once -or $Report) { Write-Output $line }
}

# ---- config ----------------------------------------------------------------
# key=value, UTF-8 with no BOM. Read every key and write every key back, so a
# hand-added one survives our rewrites of ip.
function Read-Conf {
  $c = @{}
  if (Test-Path $ConfPath) {
    foreach ($l in (Get-Content $ConfPath)) {
      $l = $l.TrimStart([char]0xFEFF)
      $i = $l.IndexOf('=')
      if ($i -gt 0) { $c[$l.Substring(0, $i)] = $l.Substring($i + 1) }
    }
  }
  return $c
}

function Write-Conf {
  param([hashtable]$Conf)
  $lines = @()
  foreach ($k in $Conf.Keys) { $lines += "$k=$($Conf[$k])" }
  # WriteAllText with no encoding argument is UTF-8 with no BOM on .NET Framework,
  # which is what a key=value file needs — a BOM renames the first key, and
  # Set-Content -Encoding UTF8 writes one on PowerShell 5.1.
  [System.IO.File]::WriteAllText($ConfPath, ($lines -join "`n") + "`n")
}

# ---- the board -------------------------------------------------------------
# Per-session state and the epoch it started, kept across ticks so "minutes in
# this state" means that rather than "minutes since the last write". One line per
# session: sessionId|state|since
function Read-State {
  $st = @{}
  if (Test-Path $StatePath) {
    foreach ($l in (Get-Content $StatePath)) {
      $p = $l -split '\|'
      if ($p.Count -ge 3) { $st[$p[0]] = @{ s = $p[1]; since = [long]$p[2] } }
    }
  }
  return $st
}

function Write-State {
  param([hashtable]$State)
  $lines = @()
  foreach ($k in $State.Keys) { $lines += "$k|$($State[$k].s)|$($State[$k].since)" }
  [System.IO.File]::WriteAllText($StatePath, ($lines -join "`n") + "`n")
}

# Is the pid in a session file still the process that wrote it?
#
# procStart is in the session file precisely because a pid alone is not an
# identity: Windows recycles them, and a recycled pid would put a dead session on
# the board forever. Compared with a two-second tolerance because the two clocks
# are written by different code paths.
function Test-SessionAlive {
  param([int]$ProcId, [string]$ProcStart)
  if ($ProcId -le 0) { return $false }
  $p = Get-Process -Id $ProcId -ErrorAction SilentlyContinue
  if (-not $p) { return $false }
  if (-not $ProcStart) { return $true }
  try {
    $want = [DateTime]::FromFileTime([long]$ProcStart)
    return ([math]::Abs(($p.StartTime - $want).TotalSeconds) -lt 2)
  } catch { return $true }
}

# A name the panel can render, with the part that makes it unique kept.
#
# Claude Code names a session "<project>-<2 hex>" and the suffix is the whole of
# what tells two windows on one project apart. Three sessions on stock-pred-v3
# clipped to twelve characters are three rows reading "stock-pred-v", which is the
# bug 0.3.2 fixed for the hook reporter — so clip the BASE and keep the suffix:
# stock-pred-v3-f7 -> stock-pre-f7.
function Clip-Name {
  param([string]$Name, [int]$Max)
  # Plain string replaces, not a regex character class: the class this needs is
  # an unterminated escape and -replace throws on it.
  $n = $Name.Replace('|', '').Replace('"', '').Replace('\', '')
  if ($n.Length -le $Max) { return $n }
  $m = [regex]::Match($n, '^(.*)-([0-9a-fA-F]{2})$')
  if ($m.Success) {
    $sfx  = '-' + $m.Groups[2].Value
    $base = $m.Groups[1].Value
    $room = $Max - $sfx.Length
    if ($room -lt 1) { return $n.Substring(0, $Max) }
    if ($base.Length -gt $room) { $base = $base.Substring(0, $room) }
    # Trailing separator trimmed, or obsidian-vault-62 clips to "obsidian--62".
    return $base.TrimEnd('-') + $sfx
  }
  return $n.Substring(0, $Max)
}

# The transcript for a session, or $null. One glob per session rather than a
# recursive walk of ~/.claude/projects, which on a machine with a year of
# projects in it is thousands of files we would restat every second.
$TranscriptCache = @{}
function Get-Transcript {
  param([string]$SessionId)
  if (-not (Test-Path $ProjDir)) { return $null }
  # Cached because a session's transcript path never changes, and the wildcard
  # glob over every project directory was most of the cost of a tick: with four
  # sessions open a tick took about four seconds, against a target of one. Only
  # the path is cached — the file is re-stat'ed every time, which is the whole
  # point of consulting it.
  $hit = $TranscriptCache[$SessionId]
  if ($hit -and (Test-Path $hit)) { return Get-Item $hit -ErrorAction SilentlyContinue }

  $f = Get-ChildItem -Path (Join-Path $ProjDir "*\$SessionId.jsonl") -File -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  if ($f) { $TranscriptCache[$SessionId] = $f.FullName }
  return $f
}

# Everything Claude Code is doing on this machine, as board rows.
#
# The state comes from one of two places and the order is the design:
#   a fresh hook state file wins, because a hook was TOLD (and only a hook can
#   see a permission prompt);
#   otherwise the transcript's mtime decides — Claude Code appends to it every
#   few seconds through a turn and goes quiet the moment the turn ends, so
#   "written in the last WORKING_SEC" is "the model holds the turn" with no
#   parsing at all.
function Build-Board {
  $now   = Now-Epoch
  $state = Read-State
  $seen  = @{}
  $rows  = @()

  if (Test-Path $SessDir) {
    foreach ($f in (Get-ChildItem (Join-Path $SessDir '*.json') -File -ErrorAction SilentlyContinue)) {
      $raw = Get-Content $f.FullName -Raw
      if (-not $raw) { continue }

      $sid = [regex]::Match($raw, '"sessionId"\s*:\s*"([0-9a-fA-F-]{36})"').Groups[1].Value
      if (-not $sid) { continue }
      $procId = 0
      $pidTxt = [regex]::Match($raw, '"pid"\s*:\s*(\d+)').Groups[1].Value
      if ($pidTxt) { $procId = [int]$pidTxt }
      $pstart = [regex]::Match($raw, '"procStart"\s*:\s*"(\d+)"').Groups[1].Value
      # The session file outlives the process it describes, so a dead pid here is
      # the ordinary case for anything closed since the last reboot, not an error.
      if (-not (Test-SessionAlive -ProcId $procId -ProcStart $pstart)) { continue }
      if ($seen.ContainsKey($sid)) { continue }
      $seen[$sid] = $true

      # Claude Code's own label ("stoplicht-3c") when it has one: already short,
      # already suffixed, and already what the user sees in their own UI. Falling
      # back to the cwd basename plus two characters of the session id reproduces
      # the same shape for a session that has not been named yet.
      $name = [regex]::Match($raw, '"name"\s*:\s*"([^"]{1,64})"').Groups[1].Value
      if (-not $name) {
        $cwd  = [regex]::Match($raw, '"cwd"\s*:\s*"([^"]+)"').Groups[1].Value
        $base = ($cwd -replace '[\\/]+$', '') -replace '.*[\\/]', ''
        if (-not $base) { $base = 'claude' }
        $name = $base + '-' + $sid.Substring(0, 2)
      }
      $name = Clip-Name -Name $name -Max $NAMELEN

      # 1. A hook's word, if it is recent enough to still be about now.
      $st   = $null
      $hook = Join-Path $HookDir $sid
      if (Test-Path $hook) {
        $hage = $now - (File-Epoch (Get-Item $hook))
        if ($hage -le $HOOK_TRUST) {
          $hp = (Get-Content $hook -First 1) -split '\|'
          if ($hp.Count -ge 3) { $st = $hp[1] }
        }
      }

      # 2. Otherwise the transcript's own clock.
      if (-not $st) {
        $tr = Get-Transcript -SessionId $sid
        if ($tr) {
          $tage = $now - (File-Epoch $tr)
          if ($tage -gt $DEAD_SEC) { continue }   # process alive, session idle for good
          if ($tage -le $WORKING_SEC) { $st = 'w' } else { $st = 'a' }
        } else {
          # A session that has not written a transcript yet is waiting on its
          # first prompt, which is exactly what 'a' means.
          $st = 'a'
        }
      }

      # Time in this state, from our own record of when it changed.
      $since = $now
      if ($state.ContainsKey($sid) -and $state[$sid].s -eq $st) { $since = [long]$state[$sid].since }
      $state[$sid] = @{ s = $st; since = $since }

      $mins = [math]::Floor(($now - $since) / 60)
      if ($mins -lt 0)     { $mins = 0 }
      if ($mins -gt 65535) { $mins = 65535 }

      $rows += [pscustomobject]@{ n = $name; s = $st; t = [int]$mins }
    }
  }

  # Forget sessions that are gone, so the state file cannot grow without bound.
  foreach ($k in @($state.Keys)) { if (-not $seen.ContainsKey($k)) { $state.Remove($k) } }
  Write-State $state

  # Most urgent first, so truncating to six rows can only drop the calm ones.
  $rank   = @{ 'b' = 0; 'a' = 1; 'w' = 2 }
  $sorted = @($rows | Sort-Object @{Expression={$rank[$_.s]}}, @{Expression={-$_.t}})
  $shown  = @($sorted | Select-Object -First $MAXROWS)

  $parts = @()
  foreach ($r in $shown) { $parts += '{"n":"' + $r.n + '","s":"' + $r.s + '","t":' + $r.t + '}' }
  return '"sess":[' + ($parts -join ',') + '],"ns":' + $rows.Count
}

# The board plus the sender metadata: what actually goes on the wire.
#
# Split from Build-Board on purpose. ts changes every second, so a whole-payload
# comparison is never equal and the "push only when something changed" rule
# silently became "push every tick" — measured, it pushed three times in twelve
# seconds with an unchanged board. Change detection compares the board; the clock
# rides along with whatever gets sent.
#
# ts/tzo hand the device a wall clock it has no other way to get: no NTP path, no
# POSIX TZ rule the recipient never set, no DST table. hb tells it how long
# silence is allowed to last, and p is where to ask for a push.
function Wrap-Board {
  param([string]$Core)
  $tzo = [int][math]::Round(([TimeZoneInfo]::Local.GetUtcOffset([DateTime]::UtcNow)).TotalMinutes)
  return '{' + $Core + ',"ts":' + (Now-Epoch) + ',"tzo":' + $tzo +
         ',"hb":' + $HB + ',"p":' + $Port + '}'
}

# ---- transport -------------------------------------------------------------
# A POST to a device that is not there burns the whole timeout instead of taking
# the instant refusal (measured 1,028 ms against 1-14 ms), so probe the port first.
function Send-Board {
  param([string]$Addr, [string]$Json)
  if (-not $Addr) { return $false }
  $h = $Addr; $p = 80
  $c = $Addr.LastIndexOf(':')
  if ($c -gt 0 -and $Addr.Substring($c + 1) -match '^\d+$') {
    $h = $Addr.Substring(0, $c); $p = [int]$Addr.Substring($c + 1)
  }
  try {
    $t  = New-Object System.Net.Sockets.TcpClient
    $ar = $t.BeginConnect($h, $p, $null, $null)
    $open = $false
    if ($ar.AsyncWaitHandle.WaitOne(300)) { $t.EndConnect($ar); $open = $true }
    $t.Close()
    if (-not $open) { return $false }
  } catch { return $false }
  try {
    Invoke-RestMethod -Uri "http://$Addr/api/usage" -Method Post -Body $Json `
      -ContentType 'application/json' -TimeoutSec 3 -ErrorAction Stop | Out-Null
    return $true
  } catch { return $false }
}

# Does this address answer as the unit we are paired with?
function Test-Unit {
  param([string]$Addr, [string]$Unit)
  try {
    $r = Invoke-RestMethod -Uri "http://$Addr/api/status" -TimeoutSec 2 -ErrorAction Stop
    if ($r.fw -ne 'clawdmeter') { return $false }
    if ($Unit -and $r.host -and $r.host -ne $Unit) { return $false }
    return $true
  } catch { return $false }
}

# Ask the finder where the unit went. Narrow (this /24, ~5 s) or wide (the
# enclosing /20, ~28 s measured) — the caller alternates, because on this SSID
# both ends roam across /24s and a narrow sweep then cannot succeed at all, while
# a wide one every minute would stall this loop for half its life.
function Find-Unit {
  param([string]$Unit, [bool]$Widen = $false)
  $finder = Join-Path $PSScriptRoot 'clawd-find.ps1'
  if (-not (Test-Path $finder)) { return $null }
  $out = ''
  try {
    $params = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $finder, '-Resolve', $Unit)
    if ($Widen) { $params += '-Wide' }
    $out = (& powershell.exe @params 2>$null | Select-Object -First 1)
  } catch {}
  if ($out) { return $out.Trim() }
  return $null
}

# ---- the listener ----------------------------------------------------------
# TcpListener, not HttpListener: an HttpListener prefix wider than localhost needs
# an admin-registered URL ACL (netsh http add urlacl), and this has to install
# without elevation. One request line in, a fixed response out — that is the whole
# protocol, because the answer is a push arriving separately.
function Start-Listener {
  param([int]$P)
  try {
    $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Any, $P)
    $l.Start()
    Write-Log "listening on 0.0.0.0:$P"
    return $l
  } catch {
    Write-Log "listener on $P unavailable: $($_.Exception.Message)"
    return $null
  }
}

# Returns the caller's IP when it asked for a refresh, else $null.
function Poll-Listener {
  param($Listener)
  if (-not $Listener) { return $null }
  $who = $null
  while ($Listener.Pending()) {
    $client = $null
    try {
      $client = $Listener.AcceptTcpClient()
      $client.ReceiveTimeout = 400
      $client.SendTimeout    = 400
      $remote = $client.Client.RemoteEndPoint.Address.ToString()
      $ns = $client.GetStream()
      $sr = New-Object System.IO.StreamReader($ns)
      $line = $sr.ReadLine()
      $payload = "ok`n"
      $resp = "HTTP/1.1 200 OK`r`nContent-Type: text/plain`r`nContent-Length: " +
              $payload.Length + "`r`nConnection: close`r`n`r`n" + $payload
      $bytes = [System.Text.Encoding]::ASCII.GetBytes($resp)
      $ns.Write($bytes, 0, $bytes.Length)
      $ns.Flush()
      if ($line -and $line -match '^(GET|POST)\s+/refresh') { $who = $remote }
    } catch {} finally {
      if ($client) { try { $client.Close() } catch {} }
    }
  }
  return $who
}

# ---- install / uninstall ---------------------------------------------------
$RunKey  = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$RunName = 'clawd-agent'

function Stop-Agent {
  $stopped = $false
  if (Test-Path $PidPath) {
    $old = 0
    try { $old = [int](Get-Content $PidPath -First 1) } catch {}
    if ($old -gt 0) {
      $p = Get-Process -Id $old -ErrorAction SilentlyContinue
      # Name-checked before killing: the pid file can outlive us and Windows
      # recycles pids, so an unchecked Stop-Process here could kill anything.
      if ($p -and $p.ProcessName -match 'powershell|pwsh') {
        Stop-Process -Id $old -Force -ErrorAction SilentlyContinue
        $stopped = $true
      }
    }
    Remove-Item $PidPath -Force -ErrorAction SilentlyContinue
  }
  return $stopped
}

function Install-Agent {
  New-Item -ItemType Directory -Force $BinDir | Out-Null

  # Copied out of the plugin, not referenced inside it. A plugin's install path is
  # version-pinned (~/.claude/plugins/cache/<mp>/<plugin>/<version>/), so a Run
  # entry pointing into it breaks silently on the next version bump and again if
  # the plugin is disabled.
  foreach ($n in @('clawd-agent.ps1', 'clawd-find.ps1')) {
    $src = Join-Path $PSScriptRoot $n
    if (Test-Path $src) { Copy-Item $src (Join-Path $BinDir $n) -Force }
  }
  $agent = Join-Path $BinDir 'clawd-agent.ps1'

  # A .vbs shim, because powershell.exe -WindowStyle Hidden still flashes a
  # console window at logon. WScript.Shell.Run with intWindowStyle 0 does not.
  $shim = Join-Path $BinDir 'clawd-agent.vbs'
  $vbs  = 'CreateObject("WScript.Shell").Run "powershell.exe -NoProfile -ExecutionPolicy Bypass -File ""' +
          $agent + '""", 0, False'
  [System.IO.File]::WriteAllText($shim, $vbs + "`r`n")

  Set-ItemProperty -Path $RunKey -Name $RunName -Value ('wscript.exe "' + $shim + '"') -Force
  Write-Output "installed: $shim"
  Write-Output "autostart: HKCU\...\Run\$RunName"

  Stop-Agent | Out-Null
  Start-Process -FilePath 'wscript.exe' -ArgumentList ('"' + $shim + '"') -WindowStyle Hidden
  Write-Output 'started'
}

function Uninstall-Agent {
  if (Stop-Agent) { Write-Output 'stopped the running agent' }
  Remove-ItemProperty -Path $RunKey -Name $RunName -ErrorAction SilentlyContinue
  Remove-Item (Join-Path $BinDir 'clawd-agent.vbs') -Force -ErrorAction SilentlyContinue
  Write-Output 'autostart removed'
}

# ---- one-shot modes --------------------------------------------------------
if ($Install)   { Install-Agent;   exit 0 }
if ($Uninstall) { Uninstall-Agent; exit 0 }

$conf = Read-Conf
$unit = $conf['host']
$addr = $conf['ip']

if ($Report) {
  $running = $false
  if (Test-Path $PidPath) {
    $rp = 0
    try { $rp = [int](Get-Content $PidPath -First 1) } catch {}
    if ($rp -gt 0) { $running = [bool](Get-Process -Id $rp -ErrorAction SilentlyContinue) }
  }
  $auto = [bool](Get-ItemProperty -Path $RunKey -Name $RunName -ErrorAction SilentlyContinue)
  Write-Output "config    : $ConfPath"
  Write-Output "unit      : $unit at $addr"
  Write-Output "running   : $running"
  Write-Output "autostart : $auto"
  Write-Output "board     : $(Wrap-Board (Build-Board))"
  exit 0
}

if (-not $addr -and -not $unit) {
  Write-Log 'no config; nothing to do (run /clawd:setup)'
  exit 0
}

if ($Once) {
  $body = Wrap-Board (Build-Board)
  $ok = Send-Board $addr $body
  Write-Log "once: sent=$ok addr=$addr body=$body"
  exit 0
}

# ---- the loop --------------------------------------------------------------
# One instance per user. A second copy would double every push and fight over the
# state file, and the Run entry plus a manual start is an ordinary way to get one.
$mutex = New-Object System.Threading.Mutex($false, 'Local\clawd-agent')
if (-not $mutex.WaitOne(0)) {
  Write-Log 'another agent already holds the lock; exiting'
  exit 0
}
[System.IO.File]::WriteAllText($PidPath, "$PID`n")
Write-Log "agent up (pid $PID, unit $unit at $addr, hb ${HB}s, port $Port)"

$listener = Start-Listener -P $Port
$lastBody  = ''
$lastSent  = 0
$lastFind  = 0
$findFails = 0
try {
  while ($true) {
    $now = Now-Epoch

    # The device asking to be pushed to. Its request also carries its current
    # address, which is how a unit that came back on a new lease repairs the
    # pairing without anyone sweeping anything.
    $poke  = Poll-Listener -Listener $listener
    $force = $false
    if ($poke) {
      $force = $true
      if ($poke -ne $addr) {
        if (Test-Unit -Addr $poke -Unit $unit) {
          Write-Log "device moved: $addr -> $poke (it told us)"
          $addr = $poke
          $conf = Read-Conf
          $conf['ip'] = $poke
          Write-Conf $conf
        } else {
          Write-Log "refresh from $poke, which is not our unit; ignored"
          $force = $false
        }
      } else {
        Write-Log 'refresh requested'
      }
    }

    $core = Build-Board
    if ($force -or $core -ne $lastBody -or ($now - $lastSent) -ge $HB) {
      $body = Wrap-Board $core
      $sent = Send-Board $addr $body

      # The stored address is a DHCP lease on a device that roams, so "stopped
      # answering" is an ordinary Tuesday. Sweep for it — throttled, because a
      # genuinely absent unit would otherwise cost a sweep a second.
      if (-not $sent -and $unit -and ($now - $lastFind) -ge $REFIND_SEC) {
        $lastFind = $now
        # Alternate narrow and wide. A device that just renewed its lease is
        # usually still on our /24 and the cheap sweep gets it; one that moved to
        # another /24 of the same /20 — which is what actually happened here on
        # 2026-09-04 — is only ever found by the expensive one.
        $wide = (($findFails % 2) -eq 1)
        $findFails++
        $found = Find-Unit -Unit $unit -Widen $wide
        if ($found -and $found -ne $addr) {
          Write-Log "device moved: $addr -> $found (found by sweep, wide=$wide)"
          $addr = $found
          $conf = Read-Conf
          $conf['ip'] = $found
          Write-Conf $conf
          $sent = Send-Board $addr $body
        }
      }

      if ($sent) {
        # Logged on a real change only. A heartbeat that says the same thing as
        # the last one is not worth a line every fifteen seconds.
        if ($core -ne $lastBody) { Write-Log "pushed: $body" }
        $lastBody  = $core
        $lastSent  = $now
        $findFails = 0
      }
    }

    Start-Sleep -Milliseconds $TICK_MS
  }
} finally {
  if ($listener) { try { $listener.Stop() } catch {} }
  Remove-Item $PidPath -Force -ErrorAction SilentlyContinue
  try { $mutex.ReleaseMutex() } catch {}
  Write-Log 'agent down'
}
