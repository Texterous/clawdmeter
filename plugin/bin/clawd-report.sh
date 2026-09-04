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
MAXQUIET=300   # s — resend an unchanged board no more often than this
REFIND=600     # s — minimum gap between attempts to relocate a moved unit
MAXROWS=6      # rows the 240x240 board renders; ns carries the true count
NAMELEN=12     # what the device renders at text size 2

[ -f "$CFG" ] || exit 0
IP=$(sed -n 's/^ip=//p' "$CFG" | head -1)
UNIT=$(sed -n 's/^host=//p' "$CFG" | head -1)
[ -n "$IP" ] || [ -n "$UNIT" ] || exit 0     # not paired; nothing to do

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
  # Two windows on the same project would otherwise draw two identical rows on
  # the glass. Suffix the first two characters of the session id — the same shape
  # Claude Code uses for its own session labels (stoplicht-e6) — so they stay
  # tellable apart.
  SUFFIX=$(printf '%s' "$SID" | cut -c1-2)
  ROOM=$(( NAMELEN - 3 ))
  NAME=$(printf '%s' "$NAME" | tr -d '|\\"' | cut -c"1-$ROOM")
  NAME="${NAME}-${SUFFIX}"

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

# ---- hand over to the agent, if one is running ------------------------------
# The agent (clawd-agent.sh) owns the transport when it is installed: it holds a
# heartbeat the device measures its stale window against, and it keeps running
# with Claude Code closed. Two senders pushing the same board would fight over
# the address in the config and halve the value of the heartbeat, so this hook
# stops at writing its state file above — which is exactly the half the agent
# cannot do for itself. A hook is TOLD that a permission prompt is up; nothing
# outside this process can see it.
if [ -f "$DIR/agent.pid" ]; then
  APID=$(head -1 "$DIR/agent.pid" 2>/dev/null || echo)
  if [ -n "$APID" ] && kill -0 "$APID" 2>/dev/null; then
    # Name-checked: pids are recycled, and "some process has this pid" is not
    # "the agent is running".
    case "$(ps -p "$APID" -o comm= 2>/dev/null)" in
      *sh*) exit 0 ;;
    esac
  fi
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

# ---- send, when there is something to say ----------------------------------
# Push on CHANGE, not on a timer. The old rule was "at most one POST every 10 s";
# it throttled a burst of tool calls correctly and also swallowed the transition
# that matters most — finish a turn within 10 s of a tool call and the Stop
# event's "waiting for you" never reached the glass, leaving the board on
# "working" until something else happened to fire a hook. Comparing the body
# sends every real change at once and sends nothing while the board is identical:
# fewer POSTs, and a board that is never wrong. push.stamp holds the body last
# confirmed on the device, so its content and its mtime answer both questions.
STAMP="$DIR/push.stamp"
if [ -f "$STAMP" ]; then
  LAST=$(cat "$STAMP" 2>/dev/null)
  SMT=$(stat -c %Y "$STAMP" 2>/dev/null || stat -f %m "$STAMP" 2>/dev/null)
  if [ "$LAST" = "$BODY" ] && [ -n "$SMT" ] && [ $(( NOW - SMT )) -lt "$MAXQUIET" ]; then
    exit 0
  fi
fi

# --connect-timeout separately from -m: an absent device otherwise burns the whole
# -m budget instead of taking the instant refusal.
send() {
  [ -n "$1" ] || return 1
  curl -s --connect-timeout 0.3 -m 2 --noproxy '*' -o /dev/null -f -X POST \
    -H 'Content-Type: application/json' -d "$BODY" \
    "http://$1/api/usage" 2>/dev/null
}

SENT=0
send "$IP" && SENT=1

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
BOUNDARY=0
case "$EVT" in UserPromptSubmit|SessionStart|Stop) BOUNDARY=1 ;; esac
if [ "$SENT" -eq 0 ] && [ -n "$UNIT" ] && [ "$BOUNDARY" -eq 1 ]; then
  RSTAMP="$DIR/resolve.stamp"
  RQ=$REFIND
  if [ -f "$RSTAMP" ]; then
    RMT=$(stat -c %Y "$RSTAMP" 2>/dev/null || stat -f %m "$RSTAMP" 2>/dev/null)
    [ -n "$RMT" ] && RQ=$(( NOW - RMT ))
  fi
  if [ "$RQ" -ge "$REFIND" ]; then
    : > "$RSTAMP"
    # Pass the /24 the unit was last seen on: the right subnet in the common case
    # (same network, new lease) and the only usable one on a box where prefix
    # detection comes up empty.
    HINT=$(printf '%s' "$IP" | cut -d: -f1 | cut -d. -f1-3)
    FOUND=$(sh "$(dirname "$0")/clawd-find.sh" --resolve "$UNIT" "$HINT" 2>/dev/null | head -1)
    if [ -n "$FOUND" ] && [ "$FOUND" != "$IP" ]; then
      # Rewrite the ip= line in place, keeping every other key — including one
      # somebody added by hand. A temp file plus mv, so a failure half way through
      # cannot leave a truncated config behind.
      { sed '/^ip=/d' "$CFG"; printf 'ip=%s\n' "$FOUND"; } > "$CFG.new" 2>/dev/null \
        && mv "$CFG.new" "$CFG" 2>/dev/null
      send "$FOUND" && SENT=1
    fi
  fi
fi

# Only a confirmed delivery updates the baseline. Recording an attempt would make
# the next hook think the device already has a board it never received.
[ "$SENT" -eq 1 ] && printf '%s' "$BODY" > "$STAMP"
exit 0
