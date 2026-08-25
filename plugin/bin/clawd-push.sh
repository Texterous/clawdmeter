#!/bin/sh
# clawd-push.sh — Claude Code statusLine hook that feeds a Clawdmeter display.
#
# Claude Code hands us the session JSON on stdin, including rate_limits for
# Claude.ai subscribers. We map those four numbers onto the device's /api/usage
# contract, POST them to the unit on the LAN, and print a status line.
#
# Two rules govern everything here:
#   1. Never block. This runs on every status line render. The POST is capped at
#      one second and rediscovery is detached, because a stalled status line is
#      worse than a stale panel.
#   2. Never fail loudly. A missing device, an absent rate_limits block, or a
#      chained command that dies must still leave a usable status line behind.
#
# Config lives in ~/.clawd/config as key=value lines, written by /clawd:setup.
#
# Modes:
#   (no args)         read stdin, push, print a status line
#   --scan [prefix]   sweep a /24 and print "<ip> <host>" for every unit found
#   --discover        re-resolve the configured unit and update the config

DIR="$HOME/.clawd"
CFG="$DIR/config"

cfg_get() {
  [ -f "$CFG" ] || return 0
  while IFS= read -r line; do
    case "$line" in
      "$1"=*) printf '%s' "${line#$1=}"; return 0 ;;
    esac
  done < "$CFG"
}

# The /24 this machine is on. The UDP-connect trick needs no route parsing and
# no interface guessing, and it sends nothing — connect() on a datagram socket
# only fixes the local endpoint the kernel would use to reach that address.
local_prefix() {
  _ip=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.*[[:space:]]src[[:space:]]\([0-9.]*\).*/\1/p' | head -1)
  if [ -z "$_ip" ]; then
    _if=$(route -n get 1.1.1.1 2>/dev/null | sed -n 's/.*interface:[[:space:]]*\(.*\)/\1/p' | head -1)
    [ -n "$_if" ] && _ip=$(ipconfig getifaddr "$_if" 2>/dev/null)
  fi
  [ -n "$_ip" ] && printf '%s' "$_ip" | cut -d. -f1-3
}

# Every Clawdmeter on a /24, as "<ip> <host>". 64 wide keeps this near 2s.
scan() {
  _prefix=$1
  [ -n "$_prefix" ] || _prefix=$(local_prefix)
  [ -n "$_prefix" ] || return 1
  seq 1 254 | xargs -P 64 -I@ sh -c \
    'r=$(curl -s -m 1 "http://'"$_prefix"'.@/api/status" 2>/dev/null)
     case "$r" in
       *"\"fw\":\"clawdmeter\""*)
         h=$(printf "%s" "$r" | sed -n "s/.*\"host\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p")
         echo "'"$_prefix"'.@ ${h:-?}" ;;
     esac' 2>/dev/null
}

# mDNS first (nearly free when the resolver cooperates), then a sweep of the /24
# the unit was last seen on, then this machine's own /24 in case both moved.
find_unit() {
  _host=$1
  if curl -s -m 2 "http://${_host}.local/api/status" 2>/dev/null | grep -q "\"host\":\"${_host}\""; then
    printf '%s' "${_host}.local"; return 0
  fi
  _old=$(cfg_get ip)
  for _p in "$(printf '%s' "$_old" | cut -d. -f1-3)" "$(local_prefix)"; do
    [ -n "$_p" ] || continue
    _hit=$(scan "$_p" | grep " ${_host}\$" | head -1 | cut -d' ' -f1)
    if [ -n "$_hit" ]; then printf '%s' "$_hit"; return 0; fi
  done
  return 1
}

cfg_set_ip() {
  tmp="$DIR/config.tmp.$$"
  { grep -v '^ip=' "$CFG" 2>/dev/null; echo "ip=$1"; } > "$tmp" && mv "$tmp" "$CFG"
}

case "$1" in
  --scan)     scan "$2"; exit $? ;;
  --discover)
    h=$(cfg_get host)
    [ -n "$h" ] || exit 1
    found=$(find_unit "$h") || exit 1
    cfg_set_ip "$found"
    exit 0 ;;
esac

# ---- session board ---------------------------------------------------------
# The device's second screen: one row per live Claude Code session on this
# machine. Ported from clawdmeter-daemon's collect_sessions/_classify.
#
# Claude Code registers each running session in ~/.claude/sessions/<pid>.json and
# streams its transcript to ~/.claude/projects/<slug>/<sessionId>.jsonl. Neither
# is a documented interface, so everything here is defensive: anything odd about
# one session drops that session, never the board, and never the usage reading.
#
# Needs jq. Shell alone cannot parse a transcript line correctly, and half-parsing
# one with sed would put wrong states on the glass — so with no jq the board is
# omitted rather than guessed. The device treats a payload with no sess/ns as
# coming from a pre-board sender, which is the honest reading of "we cannot tell".

BOARD_MAX_ROWS=6
BLOCKED_AFTER=30      # s of transcript silence mid-tool => "blocked"
WORKING_STALE=300     # a "working" session this quiet is really waiting
TAIL_BYTES=32768
NAME_LEN=12           # what the device renders at text size 2

file_mtime() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null; }

# "<state> <since_epoch>" for one transcript, or nothing if it says nothing.
# fromdateiso8601 rejects fractional seconds and Claude's timestamps have them,
# so the fraction is stripped first — with a [.] character class, because a
# backslash escape does not survive both the shell and jq's string parser.
classify() {
  _p=$1; _now=$2
  _mt=$(file_mtime "$_p") || return 1
  [ -n "$_mt" ] || return 1
  # -r matters: without raw output jq returns a quoted JSON string, and the
  # caller's arithmetic then chokes on the trailing quote.
  tail -c "$TAIL_BYTES" "$_p" 2>/dev/null | jq -rRn \
      --argjson now "$_now" --argjson mtime "$_mt" \
      --argjson blocked "$BLOCKED_AFTER" --argjson stale "$WORKING_STALE" '
    def ts: if . then (sub("[.][0-9]+Z$";"Z") | fromdateiso8601?) else null end;
    [inputs | fromjson? | select(.type=="user" or .type=="assistant")] as $es
    | ($es | last) as $last
    | ($es | map(select(.type=="user" and .origin.kind=="human")) | last) as $lp
    | if $last == null then empty
      else
        ($now - $mtime) as $silent
        | (($lp.timestamp | ts) // $mtime) as $turn
        | if $last.type == "assistant" then
            if $last.message.stop_reason == "tool_use" then
              # A tool call with no result behind it: running, or waiting on you.
              (if $silent >= $blocked then "b \($mtime)" else "w \($turn)" end)
            else "a \((($last.timestamp | ts) // $mtime))" end
          else
            # A user message last: a fresh prompt, or a tool result coming back.
            (if $silent >= $stale then "a \((($last.timestamp | ts) // $mtime))"
             else "w \($turn)" end)
          end
      end' 2>/dev/null
}

# Prints "<live_count><TAB>[<sess json array>]", or nothing.
session_board() {
  _now=$1
  _sd="$HOME/.claude/sessions"
  _pd="$HOME/.claude/projects"
  [ -d "$_sd" ] || return 1
  command -v jq >/dev/null 2>&1 || return 1

  # One jq spawn for the whole registry. These files are single-line JSON with no
  # trailing newline, so they cannot be read line by line — but jq's parser reads
  # a concatenated stream of values happily. One spawn instead of twenty is the
  # whole point: process creation, not parsing, is what this loop costs.
  _regs=$(cat "$_sd"/*.json 2>/dev/null | jq -r '
      [(.pid // ""), (.sessionId // ""),
       (.name // (.cwd // "" | split("/") | last) // "")] | @tsv' 2>/dev/null)
  [ -n "$_regs" ] || return 1

  _idx=$(mktemp 2>/dev/null) || return 1
  find "$_pd" -name '*.jsonl' 2>/dev/null > "$_idx"

  # The loop prints and the parent captures, rather than assigning inside a
  # pipeline: `while read` on the right of a pipe runs in a subshell, so anything
  # accumulated in there is discarded the moment the loop ends.
  _tab=$(printf '	')
  _rows=$(printf '%s
' "$_regs" | while IFS="$_tab" read -r _pid _sid _nm; do
    [ -n "$_pid" ] && [ -n "$_sid" ] || continue
    # Registry files outlive their process, so liveness is the real filter.
    kill -0 "$_pid" 2>/dev/null || continue
    _tp=$(grep -F "$_sid.jsonl" "$_idx" 2>/dev/null | head -1)
    [ -n "$_tp" ] || continue
    _st=$(classify "$_tp" "$_now") || continue
    [ -n "$_st" ] || continue
    _state=${_st%% *}; _since=${_st##* }

    _mins=$(( (_now - _since) / 60 ))
    [ "$_mins" -lt 0 ] && _mins=0
    [ "$_mins" -gt 65535 ] && _mins=65535
    [ -n "$_nm" ] || _nm=$(printf '%s' "$_sid" | cut -c1-8)
    _nm=$(printf '%s' "$_nm" | cut -c"1-$NAME_LEN")
    # Strip rather than escape the two characters that would break the JSON.
    # Nested backslash escaping through sh and sed is a reliable way to emit a
    # malformed payload, and the name is already truncated to 12 characters, so
    # dropping a stray quote from a pathological one loses nothing real.
    _nmj=$(printf '%s' "$_nm" | tr -d '"\\')
    case "$_state" in b) _rank=0 ;; a) _rank=1 ;; *) _rank=2 ;; esac
    # Most urgent first; the second key is 65535-mins so a plain ascending sort
    # puts the longest-waiting first within a state. Truncating to six rows can
    # then only ever drop the calmest ones.
    printf '%s %05d {"n":"%s","s":"%s","t":%s}
'       "$_rank" "$((65535 - _mins))" "$_nmj" "$_state" "$_mins"
  done)
  rm -f "$_idx"

  _rows=$(printf '%s' "$_rows" | grep -v '^[[:space:]]*$')
  # No `|| echo 0` here: grep -c PRINTS 0 and also EXITS 1 on no match, so the
  # fallback ran too and _n became "0\n0" — which cut then split across two
  # fields and put a raw newline in the middle of the JSON body.
  _n=$(printf '%s' "$_rows" | grep -c '^[0-9]' 2>/dev/null)
  _n=${_n:-0}
  _sess=$(printf '%s' "$_rows" | sort -k1,1n -k2,2n | head -"$BOARD_MAX_ROWS"           | sed 's/^[0-9]* [0-9]* //' | paste -sd, - 2>/dev/null)
  printf '%s	[%s]' "$_n" "$_sess"
}

# ---- normal mode: status line -----------------------------------------------
RAW=$(cat)
IP=$(cfg_get ip)
HOST=$(cfg_get host)
CHAIN=$(cfg_get chain)

# jq when it exists, otherwise sed. The sed path is safe because neither
# five_hour nor seven_day contains a nested object, so [^}]* cannot overrun.
extract() {
  _obj=$(printf '%s' "$RAW" | sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*{\([^}]*\)}.*/\1/p")
  _pct=$(printf '%s' "$_obj" | sed -n 's/.*"used_percentage"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p')
  _rst=$(printf '%s' "$_obj" | sed -n 's/.*"resets_at"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p')
  printf '%s %s' "${_pct:-}" "${_rst:-}"
}

if command -v jq >/dev/null 2>&1; then
  S=$(printf '%s' "$RAW"  | jq -r '.rate_limits.five_hour.used_percentage // empty' 2>/dev/null)
  SA=$(printf '%s' "$RAW" | jq -r '.rate_limits.five_hour.resets_at // empty' 2>/dev/null)
  W=$(printf '%s' "$RAW"  | jq -r '.rate_limits.seven_day.used_percentage // empty' 2>/dev/null)
  WA=$(printf '%s' "$RAW" | jq -r '.rate_limits.seven_day.resets_at // empty' 2>/dev/null)
else
  set -- $(extract five_hour); S=${1:-}; SA=${2:-}
  set -- $(extract seven_day); W=${1:-}; WA=${2:-}
fi

NOW=$(date +%s 2>/dev/null || echo 0)
mins() {
  [ -n "$1" ] || { echo 0; return; }
  _m=$(( ($1 - NOW) / 60 ))
  [ "$_m" -gt 0 ] && echo "$_m" || echo 0
}
SR=$(mins "$SA")
WR=$(mins "$WA")

# The device keeps its last good values on ok:false and flags the error, so an
# absent rate_limits block is reported honestly rather than sent as zeroes.
PUSHED=0
if [ -n "$IP" ] && command -v curl >/dev/null 2>&1; then
  if [ -n "$S" ]; then
    if   [ "$S" -ge 100 ]; then ST=rejected
    elif [ "$S" -ge 80  ]; then ST=allowed_warning
    else                        ST=allowed
    fi
    BODY="{\"s\":${S},\"sr\":${SR},\"w\":${W:-0},\"wr\":${WR},\"st\":\"${ST}\",\"ok\":true"
    # sess/ns are optional by contract — a payload without them parses exactly as
    # the usage-only one. But a unit in sessions mode reads their absence as "the
    # sender is too old", so omit them only when the board is off or jq is absent.
    if [ "$(cfg_get board)" != "0" ]; then
      BOARD=$(session_board "$NOW" 2>/dev/null)
      if [ -n "$BOARD" ]; then
        NS=$(printf '%s' "$BOARD" | cut -f1)
        SESS=$(printf '%s' "$BOARD" | cut -f2)
        BODY="${BODY},\"sess\":${SESS},\"ns\":${NS}"
      fi
    fi
    BODY="${BODY}}"
  else
    BODY='{"ok":false}'
  fi
  # --noproxy: a unit on the LAN never goes through one, and honouring
  # http_proxy here would send the reading to a proxy instead of the device.
  if curl -s -m 1 --noproxy '*' -o /dev/null -X POST \
       -H 'Content-Type: application/json' -d "$BODY" \
       "http://${IP}/api/usage" 2>/dev/null; then
    PUSHED=1
  fi
fi

# Rediscover on failure, detached, at most once a minute. A DHCP lease change or
# an AP reboot moves the unit; doing this in the background means the sweep cost
# lands on the NEXT render, not this one.
if [ "$PUSHED" -eq 0 ] && [ -n "$HOST" ]; then
  STAMP="$DIR/rediscover.stamp"
  DUE=1
  if [ -f "$STAMP" ]; then
    LAST=$(cat "$STAMP" 2>/dev/null || echo 0)
    [ $(( NOW - LAST )) -lt 60 ] && DUE=0
  fi
  if [ "$DUE" -eq 1 ]; then
    mkdir -p "$DIR" 2>/dev/null
    echo "$NOW" > "$STAMP" 2>/dev/null
    ( "$0" --discover ) >/dev/null 2>&1 &
  fi
fi

# A chained command owns the line if the user already had one; we only ever
# append our own segment. Ours is deliberately terse: the panel is the display.
if [ -n "$S" ]; then
  SEG="clawd 5h ${S}% | 7d ${W:-0}%"
else
  SEG="clawd waiting"
fi
[ "$PUSHED" -eq 0 ] && SEG="$SEG (offline)"

if [ -n "$CHAIN" ]; then
  OUT=$(printf '%s' "$RAW" | sh -c "$CHAIN" 2>/dev/null)
  if [ -n "$OUT" ]; then
    printf '%s  %s\n' "$OUT" "$SEG"
    exit 0
  fi
fi
printf '%s\n' "$SEG"
