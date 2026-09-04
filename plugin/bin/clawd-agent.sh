#!/bin/sh
# clawd-agent.sh — the always-on sender behind a Clawdmeter display (macOS, Linux).
#
# The POSIX half of clawd-agent.ps1. Same job, same payload contract, one
# deliberate difference: NO LISTENER.
#
# The Windows agent listens on a tcp port so a rebooted device can ask to be
# pushed to (GET /refresh) and be showing live rows about a second later. Doing
# that here would mean depending on `nc -l`, whose flags differ between the BSD
# netcat on macOS, the GNU one on most Linuxes, and the busybox one on the rest —
# a portability trap in the one component that has to keep running for weeks. So
# this version answers the same question with the heartbeat instead: a boot costs
# up to HB seconds of the restored board rather than about one second of it. The
# device shows real rows either way; only the age of them differs. `p` is left
# out of the payload for the same reason, so the device does not spend 800 ms a
# minute knocking on a port nobody is behind.
#
# Everything else matches: state derived from ~/.claude/sessions plus transcript
# mtimes with no hooks, a hook's state file preferred when it is fresh, push on
# change plus a heartbeat, and re-find the unit when its address stops answering.
#
# Usage:
#   clawd-agent.sh              run the loop in the foreground
#   clawd-agent.sh --install    install a login service (launchd/systemd) + start
#   clawd-agent.sh --uninstall  stop and remove it
#   clawd-agent.sh --once       build a board, push it, print the result, exit
#   clawd-agent.sh --report     print state and the board it would send

set -u

CLAWD_DIR="$HOME/.clawd"
CONF="$CLAWD_DIR/config"
HOOK_DIR="$CLAWD_DIR/sessions"
STATE="$CLAWD_DIR/agent.state"
LOG="$CLAWD_DIR/agent.log"
PIDF="$CLAWD_DIR/agent.pid"
BIN_DIR="$CLAWD_DIR/bin"
SESS_DIR="$HOME/.claude/sessions"
PROJ_DIR="$HOME/.claude/projects"

HB=15            # s — heartbeat; the device trusts 3 of these plus grace
WORKING_SEC=25   # transcript touched this recently => the model holds the turn
HOOK_TRUST=90    # s — a hook state file this fresh outranks our inference
DEAD_SEC=900     # s — a transcript quiet this long is not a live session
MAXROWS=6
NAMELEN=12
REFIND_SEC=60
LOG_MAX=65536

mkdir -p "$CLAWD_DIR" "$HOOK_DIR" 2>/dev/null || true

now_epoch() { date +%s; }

# mtime in epoch seconds, on both stats.
#
# BSD stat (macOS) wants -f %m and GNU stat (Linux) wants -c %Y, and the naive
# `stat -f %m || stat -c %Y` is WRONG rather than merely inelegant: on GNU
# coreutils -f means --file-system, so it EXITS ZERO while printing a filesystem
# report, the fallback never runs, and every age computed from it is garbage.
# Caught on this box 2026-09-04, where the whole board came back empty.
#
# So: probe once, validate that the output is a number rather than trusting the
# exit status, and try the GNU form first — BSD stat rejects -c outright, which
# is the failure mode that is safe to fall through.
STAT_MODE=""
file_epoch() {
  [ -f "$1" ] || { echo 0; return; }
  if [ -z "$STAT_MODE" ]; then
    v=$(stat -c %Y "$1" 2>/dev/null)
    case "$v" in ''|*[!0-9]*) ;; *) STAT_MODE=gnu ;; esac
    if [ -z "$STAT_MODE" ]; then
      v=$(stat -f %m "$1" 2>/dev/null)
      case "$v" in ''|*[!0-9]*) ;; *) STAT_MODE=bsd ;; esac
    fi
    [ -z "$STAT_MODE" ] && STAT_MODE=perl
  fi
  case "$STAT_MODE" in
    gnu) stat -c %Y "$1" 2>/dev/null || echo 0 ;;
    bsd) stat -f %m "$1" 2>/dev/null || echo 0 ;;
    # Neither stat understood us. perl ships with macOS and every mainstream
    # Linux, and this is one call a second at worst.
    *)   perl -e 'print((stat($ARGV[0]))[9])' "$1" 2>/dev/null || echo 0 ;;
  esac
}

log() {
  # Truncate by rewriting the tail: a log that grows without bound on a desk toy
  # is a bug report waiting to happen.
  if [ -f "$LOG" ]; then
    sz=$(wc -c < "$LOG" 2>/dev/null || echo 0)
    if [ "$sz" -gt "$LOG_MAX" ]; then
      tail -n 200 "$LOG" > "$LOG.tmp" 2>/dev/null && mv "$LOG.tmp" "$LOG"
    fi
  fi
  printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$1" >> "$LOG"
}

conf_get() {
  [ -f "$CONF" ] || return 0
  # Strip a BOM if some editor added one: it would rename the first key.
  sed -e '1s/^\xEF\xBB\xBF//' "$CONF" | awk -F= -v k="$1" '$1==k {sub(/^[^=]*=/,""); print; exit}'
}

conf_set_ip() {
  [ -f "$CONF" ] || return 0
  tmp="$CONF.tmp"
  if grep -q '^ip=' "$CONF" 2>/dev/null; then
    sed -e "s|^ip=.*|ip=$1|" "$CONF" > "$tmp" && mv "$tmp" "$CONF"
  else
    { cat "$CONF"; printf 'ip=%s\n' "$1"; } > "$tmp" && mv "$tmp" "$CONF"
  fi
}

# ---- board -----------------------------------------------------------------
# A name the panel can render with the part that makes it unique kept: Claude Code
# names a session "<project>-<2 hex>" and three windows on one project clipped to
# twelve characters are three identical rows. Clip the base, keep the suffix.
clip_name() {
  n=$(printf '%s' "$1" | tr -d '|"\\')
  len=${#n}
  [ "$len" -le "$NAMELEN" ] && { printf '%s' "$n"; return; }
  sfx=$(printf '%s' "$n" | sed -n 's/.*\(-[0-9a-fA-F][0-9a-fA-F]\)$/\1/p')
  if [ -n "$sfx" ]; then
    room=$((NAMELEN - 3))
    base=$(printf '%s' "$n" | sed 's/-[0-9a-fA-F][0-9a-fA-F]$//' | cut -c1-"$room" | sed 's/-*$//')
    printf '%s%s' "$base" "$sfx"
  else
    printf '%s' "$n" | cut -c1-"$NAMELEN"
  fi
}

json_str() { sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -1; }
json_num() { sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$1" | head -1; }

# Live sessions, one "state|since|name" line each, on stdout.
scan_sessions() {
  now=$1
  [ -d "$SESS_DIR" ] || return 0
  for f in "$SESS_DIR"/*.json; do
    [ -f "$f" ] || continue
    sid=$(json_str "$f" sessionId)
    [ -n "$sid" ] || continue
    spid=$(json_num "$f" pid)
    [ -n "$spid" ] || continue
    # The session file outlives the process it describes, so a dead pid is the
    # ordinary case for anything closed since the last reboot, not an error.
    kill -0 "$spid" 2>/dev/null || continue

    name=$(json_str "$f" name)
    if [ -z "$name" ]; then
      cwd=$(json_str "$f" cwd)
      base=$(printf '%s' "$cwd" | sed -e 's|[/\\]*$||' -e 's|.*[/\\]||')
      [ -n "$base" ] || base=claude
      name="$base-$(printf '%s' "$sid" | cut -c1-2)"
    fi
    name=$(clip_name "$name")

    # 1. A hook's word, if it is recent enough to still be about now — and only a
    #    hook can see a permission prompt.
    st=""
    hook="$HOOK_DIR/$sid"
    if [ -f "$hook" ]; then
      hage=$((now - $(file_epoch "$hook")))
      if [ "$hage" -le "$HOOK_TRUST" ]; then
        st=$(head -1 "$hook" | cut -d'|' -f2)
      fi
    fi

    # 2. Otherwise the transcript's own clock. Claude Code appends to it every few
    #    seconds through a turn and goes quiet when the turn ends.
    if [ -z "$st" ]; then
      tr=$(ls -t "$PROJ_DIR"/*/"$sid".jsonl 2>/dev/null | head -1)
      if [ -n "$tr" ]; then
        tage=$((now - $(file_epoch "$tr")))
        [ "$tage" -gt "$DEAD_SEC" ] && continue
        if [ "$tage" -le "$WORKING_SEC" ]; then st=w; else st=a; fi
      else
        st=a
      fi
    fi

    # Time in this state, from our own record of when it changed.
    since=$now
    if [ -f "$STATE" ]; then
      prev=$(awk -F'|' -v s="$sid" '$1==s {print $2"|"$3; exit}' "$STATE")
      pst=$(printf '%s' "$prev" | cut -d'|' -f1)
      psince=$(printf '%s' "$prev" | cut -d'|' -f2)
      if [ "$pst" = "$st" ] && [ -n "$psince" ]; then since=$psince; fi
    fi
    printf '%s|%s|%s|%s\n' "$sid" "$st" "$since" "$name"
  done
}

build_board() {
  now=$(now_epoch)
  rows=$(scan_sessions "$now")

  # Rewrite the state file from what we just saw, which also forgets sessions
  # that are gone so it cannot grow without bound.
  if [ -n "$rows" ]; then
    printf '%s\n' "$rows" | awk -F'|' '{print $1"|"$2"|"$3}' > "$STATE"
  else
    : > "$STATE"
  fi

  total=0
  [ -n "$rows" ] && total=$(printf '%s\n' "$rows" | grep -c .)

  # Most urgent first (blocked, awaiting, working), longest-standing first inside
  # each, so truncating to six rows can only drop the calm ones.
  parts=""
  if [ -n "$rows" ]; then
    sorted=$(printf '%s\n' "$rows" | awk -F'|' -v now="$now" '
      { r = ($2=="b") ? 0 : ($2=="a") ? 1 : 2; printf "%d %d %s %s\n", r, now-$3, $2, $4 }' |
      sort -k1,1n -k2,2nr | head -"$MAXROWS")
    oldifs=$IFS
    IFS='
'
    for line in $sorted; do
      IFS=$oldifs
      secs=$(printf '%s' "$line" | awk '{print $2}')
      stt=$(printf '%s' "$line" | awk '{print $3}')
      nm=$(printf '%s' "$line" | awk '{print $4}')
      mins=$((secs / 60))
      [ "$mins" -lt 0 ] && mins=0
      [ "$mins" -gt 65535 ] && mins=65535
      row="{\"n\":\"$nm\",\"s\":\"$stt\",\"t\":$mins}"
      if [ -z "$parts" ]; then parts="$row"; else parts="$parts,$row"; fi
      IFS='
'
    done
    IFS=$oldifs
  fi

  # ts/tzo hand the device a wall clock it has no other way to get: no NTP path,
  # no POSIX TZ rule the recipient never set, no DST table. %z is +HHMM.
  z=$(date +%z)
  sign=$(printf '%s' "$z" | cut -c1)
  zh=$(printf '%s' "$z" | cut -c2-3)
  zm=$(printf '%s' "$z" | cut -c4-5)
  tzo=$((10#$zh * 60 + 10#$zm))
  [ "$sign" = "-" ] && tzo=$((0 - tzo))

  # No "p": there is no listener in this version (see the header), and a device
  # knocking on a closed port would pay 800 ms a minute for nothing.
  printf '{"sess":[%s],"ns":%s,"ts":%s,"tzo":%s,"hb":%s}' "$parts" "$total" "$now" "$tzo" "$HB"
}

# ---- transport -------------------------------------------------------------
send_board() {
  addr=$1; body=$2
  [ -n "$addr" ] || return 1
  # --connect-timeout keeps an absent device from costing the whole budget; the
  # instant refusal is what we want and it arrives in milliseconds.
  code=$(curl -s -o /dev/null -w '%{http_code}' -m 4 --connect-timeout 1 --noproxy '*' \
         -X POST -H 'Content-Type: application/json' -d "$body" \
         "http://$addr/api/usage" 2>/dev/null || echo 000)
  [ "$code" = "200" ]
}

find_unit() {
  unit=$1; wide=$2
  finder="$(dirname "$0")/clawd-find.sh"
  [ -f "$finder" ] || return 0
  if [ "$wide" = "1" ]; then
    sh "$finder" --resolve "$unit" --wide 2>/dev/null | head -1
  else
    sh "$finder" --resolve "$unit" 2>/dev/null | head -1
  fi
}

# ---- install / uninstall ---------------------------------------------------
PLIST="$HOME/Library/LaunchAgents/com.texterous.clawd-agent.plist"
UNIT="$HOME/.config/systemd/user/clawd-agent.service"

stop_agent() {
  if [ -f "$PIDF" ]; then
    old=$(head -1 "$PIDF" 2>/dev/null || echo)
    # Killed only if it is still a shell: pids are recycled and "some process has
    # this pid" is not "the agent is running".
    if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then
      case "$(ps -p "$old" -o comm= 2>/dev/null)" in
        *sh*) kill "$old" 2>/dev/null ;;
      esac
    fi
    rm -f "$PIDF"
  fi
}

install_agent() {
  mkdir -p "$BIN_DIR"
  # Copied out of the plugin, not referenced inside it: a plugin's install path is
  # version-pinned, so a login item pointing into it breaks on the next update.
  for n in clawd-agent.sh clawd-find.sh; do
    src="$(dirname "$0")/$n"
    [ -f "$src" ] && cp "$src" "$BIN_DIR/$n" && chmod +x "$BIN_DIR/$n"
  done
  agent="$BIN_DIR/clawd-agent.sh"

  if [ -d "$HOME/Library/LaunchAgents" ] || [ "$(uname)" = "Darwin" ]; then
    mkdir -p "$HOME/Library/LaunchAgents"
    cat > "$PLIST" <<PLISTEOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>com.texterous.clawd-agent</string>
  <key>ProgramArguments</key>
  <array><string>/bin/sh</string><string>$agent</string></array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardErrorPath</key><string>$CLAWD_DIR/agent.err</string>
</dict>
</plist>
PLISTEOF
    launchctl unload "$PLIST" 2>/dev/null || true
    launchctl load "$PLIST" 2>/dev/null || true
    echo "installed: $PLIST"
    return 0
  fi

  mkdir -p "$(dirname "$UNIT")"
  cat > "$UNIT" <<UNITEOF
[Unit]
Description=Clawdmeter agent

[Service]
ExecStart=/bin/sh $agent
Restart=always
RestartSec=10

[Install]
WantedBy=default.target
UNITEOF
  systemctl --user daemon-reload 2>/dev/null || true
  systemctl --user enable --now clawd-agent.service 2>/dev/null || true
  echo "installed: $UNIT"
}

uninstall_agent() {
  if [ -f "$PLIST" ]; then
    launchctl unload "$PLIST" 2>/dev/null || true
    rm -f "$PLIST"
    echo "removed: $PLIST"
  fi
  if [ -f "$UNIT" ]; then
    systemctl --user disable --now clawd-agent.service 2>/dev/null || true
    rm -f "$UNIT"
    systemctl --user daemon-reload 2>/dev/null || true
    echo "removed: $UNIT"
  fi
  stop_agent
}

# ---- entry -----------------------------------------------------------------
MODE=loop
for a in "$@"; do
  case "$a" in
    --install)   MODE=install ;;
    --uninstall) MODE=uninstall ;;
    --once)      MODE=once ;;
    --report)    MODE=report ;;
  esac
done

case "$MODE" in
  install)   install_agent; exit 0 ;;
  uninstall) uninstall_agent; exit 0 ;;
esac

UNITNAME=$(conf_get host)
ADDR=$(conf_get ip)

if [ "$MODE" = "report" ]; then
  running=false
  if [ -f "$PIDF" ] && kill -0 "$(head -1 "$PIDF")" 2>/dev/null; then running=true; fi
  echo "config    : $CONF"
  echo "unit      : $UNITNAME at $ADDR"
  echo "running   : $running"
  echo "board     : $(build_board)"
  exit 0
fi

if [ -z "$ADDR" ] && [ -z "$UNITNAME" ]; then
  log 'no config; nothing to do (run /clawd:setup)'
  exit 0
fi

if [ "$MODE" = "once" ]; then
  body=$(build_board)
  if send_board "$ADDR" "$body"; then ok=true; else ok=false; fi
  log "once: sent=$ok addr=$ADDR body=$body"
  echo "sent=$ok addr=$ADDR"
  echo "$body"
  exit 0
fi

# ---- the loop --------------------------------------------------------------
# One instance. A second copy would double every push and fight over the state
# file, and a login item plus a manual start is an ordinary way to get one.
if [ -f "$PIDF" ] && kill -0 "$(head -1 "$PIDF" 2>/dev/null)" 2>/dev/null; then
  log 'another agent is already running; exiting'
  exit 0
fi
printf '%s\n' "$$" > "$PIDF"
trap 'rm -f "$PIDF"; log "agent down"; exit 0' INT TERM
log "agent up (pid $$, unit $UNITNAME at $ADDR, hb ${HB}s, no listener)"

LAST_BODY=""
LAST_SENT=0
LAST_FIND=0
FIND_FAILS=0
while :; do
  NOW=$(now_epoch)
  BODY=$(build_board)
  if [ "$BODY" != "$LAST_BODY" ] || [ $((NOW - LAST_SENT)) -ge "$HB" ]; then
    if send_board "$ADDR" "$BODY"; then SENT=1; else SENT=0; fi

    # The stored address is a DHCP lease on a device that roams, so "stopped
    # answering" is an ordinary Tuesday. Alternate a cheap /24 sweep with the
    # wide one: a device that moved to another /24 of the same /20 is only ever
    # found by the expensive one.
    if [ "$SENT" = "0" ] && [ -n "$UNITNAME" ] && [ $((NOW - LAST_FIND)) -ge "$REFIND_SEC" ]; then
      LAST_FIND=$NOW
      wide=0
      [ $((FIND_FAILS % 2)) -eq 1 ] && wide=1
      FIND_FAILS=$((FIND_FAILS + 1))
      found=$(find_unit "$UNITNAME" "$wide")
      if [ -n "$found" ] && [ "$found" != "$ADDR" ]; then
        log "device moved: $ADDR -> $found (found by sweep, wide=$wide)"
        ADDR=$found
        conf_set_ip "$found"
        if send_board "$ADDR" "$BODY"; then SENT=1; fi
      fi
    fi

    if [ "$SENT" = "1" ]; then
      [ "$BODY" != "$LAST_BODY" ] && log "pushed: $BODY"
      LAST_BODY=$BODY
      LAST_SENT=$NOW
      FIND_FAILS=0
    fi
  fi
  sleep 1
done
