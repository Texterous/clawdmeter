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
    BODY="{\"s\":${S},\"sr\":${SR},\"w\":${W:-0},\"wr\":${WR},\"st\":\"${ST}\",\"ok\":true}"
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
