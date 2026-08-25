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
$MAXQUIET = 300   # s — resend an unchanged board no more often than this
$REFIND   = 600   # s — minimum gap between attempts to relocate a moved unit
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
$ip   = $conf['ip']
$unit = $conf['host']
if (-not $ip -and -not $unit) { exit 0 }   # not paired; nothing to do

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
  # Two windows on the same project would otherwise draw two identical rows on
  # the glass. Suffix the first two characters of the session id — the same shape
  # Claude Code uses for its own session labels (stoplicht-e6) — so they stay
  # tellable apart.
  $suffix = '-' + $sid.Substring(0, 2)
  $room   = $NAMELEN - $suffix.Length
  if ($name.Length -gt $room) { $name = $name.Substring(0, $room) }
  $name = $name + $suffix

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

# ---- send, when there is something to say ----------------------------------
# Push on CHANGE, not on a timer. The old rule was "at most one POST every 10 s";
# it throttled a burst of tool calls correctly and also swallowed the transition
# that matters most — finish a turn within 10 s of a tool call and the Stop
# event's "waiting for you" never reached the glass, leaving the board on
# "working" until something else happened to fire a hook. Comparing the body
# sends every real change at once and sends nothing while the board is identical:
# fewer POSTs, and a board that is never wrong. push.stamp holds the body last
# confirmed on the device, so its content and its mtime answer both questions.
$stamp = Join-Path $Dir 'push.stamp'
$last  = ''
$quiet = [int]::MaxValue
if (Test-Path $stamp) {
  $last  = [System.IO.File]::ReadAllText($stamp)
  $quiet = $now - (Get-FileEpoch (Get-Item $stamp))
}
if ($body -eq $last -and $quiet -lt $MAXQUIET) { exit 0 }

# A POST to a device that is not there burns the whole timeout instead of taking
# the instant refusal (measured 1,028 ms vs 1-14 ms), so probe the port first.
function Send-Board {
  param([string]$Addr, [string]$Json)
  if (-not $Addr) { return $false }
  $h = $Addr; $port = 80
  $c = $Addr.LastIndexOf(':')
  if ($c -gt 0 -and $Addr.Substring($c + 1) -match '^\d+$') { $h = $Addr.Substring(0, $c); $port = [int]$Addr.Substring($c + 1) }
  try {
    $t = New-Object System.Net.Sockets.TcpClient
    $ar = $t.BeginConnect($h, $port, $null, $null)
    $open = $false
    if ($ar.AsyncWaitHandle.WaitOne(300)) { $t.EndConnect($ar); $open = $true }
    $t.Close()
    if (-not $open) { return $false }
  } catch { return $false }
  try {
    Invoke-RestMethod -Uri "http://$Addr/api/usage" -Method Post -Body $Json `
      -ContentType 'application/json' -TimeoutSec 2 -ErrorAction Stop | Out-Null
    return $true
  } catch { return $false }
}

$sent = Send-Board $ip $body

# The address in the config is a DHCP lease on a device that roams between
# networks, so "the stored IP stopped answering" is an ordinary Tuesday rather
# than a fault — and it used to end the pairing permanently: every later hook
# probed the same dead address and gave up, so the panel said "waiting..." for
# good and only re-running setup by hand fixed it. Ask the finder where the unit
# went, and remember the answer.
#
# Throttled hard, and only at a turn boundary. The lookup costs a few seconds; a
# genuinely absent device would otherwise pay that on every hook, and a hook fires
# per tool call. UserPromptSubmit, SessionStart and Stop are the moments a short
# pause is invisible anyway, and they are the moments a moved unit is worth
# chasing — nobody is watching the panel mid-tool-loop.
$boundary = @('UserPromptSubmit', 'SessionStart', 'Stop') -contains $evt
if (-not $sent -and $unit -and $boundary) {
  $rstamp = Join-Path $Dir 'resolve.stamp'
  $rquiet = [int]::MaxValue
  if (Test-Path $rstamp) { $rquiet = $now - (Get-FileEpoch (Get-Item $rstamp)) }
  if ($rquiet -ge $REFIND) {
    [System.IO.File]::WriteAllText($rstamp, 'x')
    $finder = Join-Path $PSScriptRoot 'clawd-find.ps1'
    $found = ''
    try {
      $found = (& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $finder -Resolve $unit 2>$null | Select-Object -First 1)
    } catch {}
    if ($found) { $found = $found.Trim() }
    if ($found -and $found -ne $ip) {
      $conf['ip'] = $found
      # Rewrite every key, so a hand-added one survives. WriteAllText with no
      # encoding argument is UTF-8 with no BOM, which is what the sh reporter's
      # sed expects — a BOM renames the first key of a key=value file.
      $lines = @()
      foreach ($k in $conf.Keys) { $lines += "$k=$($conf[$k])" }
      [System.IO.File]::WriteAllText($Cfg, ($lines -join "`n") + "`n")
      $sent = Send-Board $found $body
    }
  }
}

# Only a confirmed delivery updates the baseline. Recording an attempt would make
# the next hook think the device already has a board it never received.
if ($sent) { [System.IO.File]::WriteAllText($stamp, $body) }
exit 0
