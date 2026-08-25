<#
  clawd-report.ps1 — Claude Code hook that feeds a Clawdmeter session board.

  Every Claude Code session has this plugin, so every session reports ITSELF: the
  hook event says what this session is doing, we write one flat line about it, and
  then rebuild the whole board from those lines and POST it.

  That is why there is no JSON parser here and no dependency to install. The old
  design read other sessions' registry files and tailed their transcripts to guess
  a state from how long a file had been silent — which needed jq, and guessed. A
  hook is told: PostToolUse means working, Stop means waiting for you, and a
  permission_prompt Notification means blocked. No inference.

  Runs in every entrypoint, including the desktop app, where a statusLine never
  fires. Never blocks and never fails loudly: a missing device or an unparseable
  payload leaves the board alone rather than taking a session down with it.
#>
$ErrorActionPreference = 'SilentlyContinue'
$ProgressPreference    = 'SilentlyContinue'
try { [System.Net.WebRequest]::DefaultWebProxy = $null } catch {}

$Dir      = Join-Path $HOME '.clawd'
$SDir     = Join-Path $Dir 'sessions'
$Cfg      = Join-Path $Dir 'config'
$EXPIRE   = 900   # s — a state file untouched this long belongs to a dead session
$THROTTLE = 10    # s — minimum gap between pushes, so a burst of tools is one POST
$MAXROWS  = 6     # rows the 240x240 board renders; ns carries the true count
$NAMELEN  = 12    # what the device renders at text size 2

# $conf, not $cfg: PowerShell variable names are CASE-INSENSITIVE, so $cfg and
# $Cfg are one variable. Assigning the hashtable clobbered the config path, and
# the script then exited 0 in silence on every hook — a completely invisible
# failure. Keep these two names distinct.
$conf = @{}
if (Test-Path $Cfg) {
  foreach ($l in (Get-Content $Cfg)) {
    $l = $l.TrimStart([char]0xFEFF)
    $i = $l.IndexOf('=')
    if ($i -gt 0) { $conf[$l.Substring(0, $i)] = $l.Substring($i + 1) }
  }
}
$ip = $conf['ip']
if (-not $ip) { exit 0 }   # not paired; nothing to do

$raw = [Console]::In.ReadToEnd()

# session_id is a UUID, so this pattern cannot collide with anything nested inside
# tool_input or tool_response. The other two are matched on their value shape for
# the same reason.
$sid   = [regex]::Match($raw, '"session_id"\s*:\s*"([0-9a-fA-F-]{36})"').Groups[1].Value
$evt   = [regex]::Match($raw, '"hook_event_name"\s*:\s*"([A-Za-z]+)"').Groups[1].Value
$ntype = [regex]::Match($raw, '"notification_type"\s*:\s*"([A-Za-z_]+)"').Groups[1].Value
if (-not $sid) { exit 0 }

$now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()

# File mtime as epoch seconds. Via DateTimeOffset, never a DateTime subtraction:
# [datetime]'1970-01-01T00:00:00Z' is Kind=Local, so subtracting it silently adds
# the local UTC offset. That made every state file compute as an hour too old and
# get deleted as expired the instant it was written — the board was always empty.
function Get-FileEpoch {
  param([System.IO.FileSystemInfo]$File)
  return [long]([DateTimeOffset]::new($File.LastWriteTimeUtc, [TimeSpan]::Zero).ToUnixTimeSeconds())
}

New-Item -ItemType Directory -Force $SDir | Out-Null
$mine = Join-Path $SDir $sid

if ($evt -eq 'SessionEnd') {
  Remove-Item $mine -Force
} else {
  $state = 'w'
  if     ($evt -eq 'Stop')          { $state = 'a' }
  elseif ($evt -eq 'SessionStart')  { $state = 'a' }
  elseif ($evt -eq 'Notification')  { if ($ntype -eq 'permission_prompt') { $state = 'b' } else { $state = 'a' } }

  # From the payload, not from the hook process's working directory: that is
  # wherever the shell happened to start, which is not the session's cwd.
  $cwd = [regex]::Match($raw, '"cwd"\s*:\s*"([^"]+)"').Groups[1].Value
  if (-not $cwd) { $cwd = (Get-Location).Path }
  $name = ($cwd -replace '[\\/]+$', '') -replace '.*[\\/]', ''
  # Plain string replaces, not a regex character class: '[|"\]' is an unterminated
  # escape and -replace throws on it, which SilentlyContinue then hid.
  $name = $name.Replace('|', '').Replace('"', '').Replace('\', '')
  if ($name.Length -gt $NAMELEN) { $name = $name.Substring(0, $NAMELEN) }

  # Keep the original timestamp while the state is unchanged, so the device's
  # "minutes in this state" means that rather than "minutes since last tool call".
  $since = $now
  if (Test-Path $mine) {
    $prev = (Get-Content $mine -First 1) -split '\|'
    if ($prev.Count -ge 3 -and $prev[1] -eq $state) { $since = [long]$prev[2] }
  }
  [System.IO.File]::WriteAllText($mine, "$name|$state|$since")
}

# ---- rebuild the board from every session's line ---------------------------
$rank = @{ 'b' = 0; 'a' = 1; 'w' = 2 }
$rows = @()
foreach ($f in (Get-ChildItem $SDir -File)) {
  # File mtime is the liveness check: every event rewrites the file, so one that
  # has not been touched in EXPIRE seconds belongs to a session that is gone.
  if (($now - (Get-FileEpoch $f)) -gt $EXPIRE) {
    Remove-Item $f.FullName -Force
    continue
  }
  $p = (Get-Content $f.FullName -First 1) -split '\|'
  if ($p.Count -lt 3) { continue }
  $mins = [math]::Floor(($now - [long]$p[2]) / 60)
  if ($mins -lt 0) { $mins = 0 }
  if ($mins -gt 65535) { $mins = 65535 }
  $rows += [pscustomobject]@{ n = $p[0]; s = $p[1]; t = [int]$mins }
}

# Most urgent first, so truncating to six rows can only drop the calm ones.
$sorted = @($rows | Sort-Object @{Expression={$rank[$_.s]}}, @{Expression={-$_.t}})
$shown  = @($sorted | Select-Object -First $MAXROWS)
$parts  = @()
foreach ($r in $shown) { $parts += '{"n":"' + $r.n + '","s":"' + $r.s + '","t":' + $r.t + '}' }
$body = '{"sess":[' + ($parts -join ',') + '],"ns":' + $rows.Count + '}'

# ---- push, throttled -------------------------------------------------------
$stamp = Join-Path $Dir 'push.stamp'
if (Test-Path $stamp) {
  $age = $now - (Get-FileEpoch (Get-Item $stamp))
  if ($age -lt $THROTTLE) { exit 0 }
}
[System.IO.File]::WriteAllText($stamp, 'x')

# Probe first: a POST to a device that is not there burns the whole timeout
# instead of taking the instant refusal (measured 1,028 ms vs 1-14 ms).
$h = $ip; $port = 80
$c = $ip.LastIndexOf(':')
if ($c -gt 0 -and $ip.Substring($c + 1) -match '^\d+$') { $h = $ip.Substring(0, $c); $port = [int]$ip.Substring($c + 1) }
try {
  $t = New-Object System.Net.Sockets.TcpClient
  $ar = $t.BeginConnect($h, $port, $null, $null)
  $open = $false
  if ($ar.AsyncWaitHandle.WaitOne(300)) { $t.EndConnect($ar); $open = $true }
  $t.Close()
  if (-not $open) { exit 0 }
} catch { exit 0 }

try {
  Invoke-RestMethod -Uri "http://$ip/api/usage" -Method Post -Body $body `
    -ContentType 'application/json' -TimeoutSec 2 -ErrorAction Stop | Out-Null
} catch {}
exit 0
