#!/bin/sh
# clawd-report.sh — Claude Code hook that feeds a Clawdmeter session board.
#
# Every Claude Code session has this plugin, so every session reports ITSELF: the
# hook event says what this session is doing, we write one flat line about it, and
# then rebuild the whole board from those lines and POST it.
#
# That is why there is no jq here and nothing to install. The old design read other
# sessions' registry files and tailed their transcripts to guess a state from how
# long a file had been silent — which needed a JSON parser, and guessed. A hook is
# told: PostToolUse means working, Stop means waiting for you, and a
# permission_prompt Notification means blocked. No inference.
#
# Runs in every entrypoint, including the desktop app, where a statusLine never
# fires. Never blocks and never fails loudly.

DIR="$HOME/.clawd"
SDIR="$DIR/sessions"
CFG="$DIR/config"
EXPIRE=900     # s — a state file untouched this long belongs to a dead session
THROTTLE=10    # s — minimum gap between pushes, so a burst of tools is one POST
MAXROWS=6      # rows the 240x240 board renders; ns carries the true count
NAMELEN=12     # what the device renders at text size 2

[ -f "$CFG" ] || exit 0
IP=$(sed -n 's/^ip=//p' "$CFG" | head -1)
[ -n "$IP" ] || exit 0          # not paired; nothing to do

RAW=$(cat)

# session_id is a UUID, so this pattern cannot collide with anything nested inside
# tool_input or tool_response. The other two are matched on their value shape for
# the same reason.
SID=$(printf '%s' "$RAW" | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([0-9a-fA-F-]\{36\}\)".*/\1/p' | head -1)
EVT=$(printf '%s' "$RAW" | sed -n 's/.*"hook_event_name"[[:space:]]*:[[:space:]]*"\([A-Za-z]*\)".*/\1/p' | head -1)
NTYPE=$(printf '%s' "$RAW" | sed -n 's/.*"notification_type"[[:space:]]*:[[:space:]]*"\([A-Za-z_]*\)".*/\1/p' | head -1)
[ -n "$SID" ] || exit 0

NOW=$(date +%s)
mkdir -p "$SDIR" 2>/dev/null
MINE="$SDIR/$SID"

if [ "$EVT" = "SessionEnd" ]; then
  rm -f "$MINE"
else
  case "$EVT" in
    Stop|SessionStart) STATE=a ;;
    Notification)      if [ "$NTYPE" = "permission_prompt" ]; then STATE=b; else STATE=a; fi ;;
    *)                 STATE=w ;;
  esac

  # From the payload, not from $PWD: the hook's working directory is wherever the
  # shell happened to start, which is not the session's cwd.
  CWD=$(printf '%s' "$RAW" | sed -n 's/.*"cwd"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
  [ -n "$CWD" ] || CWD=$PWD
  # Last path component, for either separator. Then strip the two characters that
  # would break the JSON — tr with a trailing backslash in the set is itself a
  # malformed escape, so the set is spelled out via a bracket expression.
  NAME=$(printf '%s' "$CWD" | sed 's#[\/]*$##; s#.*[\/]##')
  NAME=$(printf '%s' "$NAME" | tr -d '|\\"' | cut -c"1-$NAMELEN")

  # Keep the original timestamp while the state is unchanged, so the device's
  # "minutes in this state" means that rather than "minutes since last tool call".
  SINCE=$NOW
  if [ -f "$MINE" ]; then
    PREV=$(head -1 "$MINE" 2>/dev/null)
    PSTATE=$(printf '%s' "$PREV" | cut -d'|' -f2)
    PSINCE=$(printf '%s' "$PREV" | cut -d'|' -f3)
    if [ "$PSTATE" = "$STATE" ] && [ -n "$PSINCE" ]; then SINCE=$PSINCE; fi
  fi
  printf '%s|%s|%s' "$NAME" "$STATE" "$SINCE" > "$MINE"
fi

# ---- rebuild the board from every session's line ---------------------------
ROWS=""
LIVE=0
for f in "$SDIR"/*; do
  [ -f "$f" ] || continue
  # File mtime is the liveness check: every event rewrites the file, so one that
  # has not been touched in EXPIRE seconds belongs to a session that is gone.
  MT=$(stat -c %Y "$f" 2>/dev/null || stat -f %m "$f" 2>/dev/null)
  [ -n "$MT" ] || continue
  if [ $(( NOW - MT )) -gt "$EXPIRE" ]; then rm -f "$f"; continue; fi
  LINE=$(head -1 "$f" 2>/dev/null)
  N=$(printf '%s' "$LINE" | cut -d'|' -f1)
  S=$(printf '%s' "$LINE" | cut -d'|' -f2)
  T0=$(printf '%s' "$LINE" | cut -d'|' -f3)
  [ -n "$N" ] && [ -n "$S" ] && [ -n "$T0" ] || continue
  MINS=$(( (NOW - T0) / 60 ))
  [ "$MINS" -lt 0 ] && MINS=0
  [ "$MINS" -gt 65535 ] && MINS=65535
  case "$S" in b) RANK=0 ;; a) RANK=1 ;; *) RANK=2 ;; esac
  # Second key is 65535-mins so a plain ascending sort puts the longest-waiting
  # first within a state; truncating to six can then only drop the calm ones.
  ROWS="${ROWS}${RANK} $(printf '%05d' $((65535 - MINS))) {\"n\":\"${N}\",\"s\":\"${S}\",\"t\":${MINS}}
"
  LIVE=$((LIVE + 1))
done

SESS=$(printf '%s' "$ROWS" | grep -v '^[[:space:]]*$' | sort -k1,1n -k2,2n \
       | head -"$MAXROWS" | sed 's/^[0-9]* [0-9]* //' | paste -sd, - 2>/dev/null)
BODY="{\"sess\":[${SESS}],\"ns\":${LIVE}}"

# ---- push, throttled -------------------------------------------------------
STAMP="$DIR/push.stamp"
if [ -f "$STAMP" ]; then
  SMT=$(stat -c %Y "$STAMP" 2>/dev/null || stat -f %m "$STAMP" 2>/dev/null)
  [ -n "$SMT" ] && [ $(( NOW - SMT )) -lt "$THROTTLE" ] && exit 0
fi
: > "$STAMP"

# --connect-timeout separately from -m: an absent device otherwise burns the whole
# -m budget instead of taking the instant refusal.
curl -s --connect-timeout 0.3 -m 2 --noproxy '*' -o /dev/null -X POST \
  -H 'Content-Type: application/json' -d "$BODY" \
  "http://${IP}/api/usage" 2>/dev/null
exit 0
