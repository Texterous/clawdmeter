#!/bin/sh
# clawd-find.sh — locate Clawdmeter units on the local network.
#
# Two jobs, and the second is the one that keeps a pairing alive:
#
#   (no args) / <prefix>   list every unit on a /24, as "<ip> <host>" lines.
#                          Used by /clawd:setup to pick a unit.
#   --resolve <host>       print the current IP of ONE named unit, or nothing.
#                          Used by clawd-report when the stored address stops
#                          answering, which is what a DHCP renewal looks like
#                          from here. Without this a paired unit went silent for
#                          good and the only cure was re-running setup by hand.

# The /24 this machine is on. Linux answers from `ip route get`; macOS has no such
# thing, so go by the interface the default route uses; `hostname -I` is the last
# resort for a box with neither (some containers, some minimal images).
local_prefix() {
  _ip=$(ip -4 route get 1.1.1.1 2>/dev/null | sed -n 's/.*[[:space:]]src[[:space:]]\([0-9.]*\).*/\1/p' | head -1)
  if [ -z "$_ip" ]; then
    _if=$(route -n get 1.1.1.1 2>/dev/null | sed -n 's/.*interface:[[:space:]]*\(.*\)/\1/p' | head -1)
    [ -n "$_if" ] && _ip=$(ipconfig getifaddr "$_if" 2>/dev/null)
  fi
  [ -n "$_ip" ] || _ip=$(hostname -I 2>/dev/null | tr ' ' '\n' | grep -m1 '^[0-9]')
  [ -n "$_ip" ] && printf '%s' "$_ip" | cut -d. -f1-3
}

# Every Clawdmeter on a /24, as "<ip> <host>". 64 wide keeps this near 2s.
scan() {
  _prefix=$1
  [ -n "$_prefix" ] || _prefix=$(local_prefix)
  [ -n "$_prefix" ] || return 1
  seq 1 254 | xargs -P 64 -I@ sh -c \
    'r=$(curl -s --connect-timeout 0.3 -m 1 "http://'"$_prefix"'.@/api/status" 2>/dev/null)
     case "$r" in
       *"\"fw\":\"clawdmeter\""*)
         h=$(printf "%s" "$r" | sed -n "s/.*\"host\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p")
         echo "'"$_prefix"'.@ ${h:-?}" ;;
     esac' 2>/dev/null
}

# One named unit's current IP. The /24 first — it is both the faster answer and
# the one that works when mDNS is blocked, which is the common case on a venue
# network. Then .local, which earns its keep only for a unit outside this /24.
#
# Note what the mDNS branch returns: /api/status reports the device's own numeric
# address, so a working .local lookup yields an IP with no DNS tooling at all.
# $2 is the /24 the unit was last seen on, which the caller knows and this script
# would otherwise have to guess. It is the right subnet in the common case (same
# network, new lease) and the only usable one where prefix detection fails.
resolve() {
  _host=$1
  _hint=$2
  [ -n "$_host" ] || return 1
  _auto=$(local_prefix)
  [ "$_hint" = "$_auto" ] && _auto=''      # same subnet: one sweep, not two
  for _p in "$_hint" "$_auto"; do
    [ -n "$_p" ] || continue
    _ip=$(scan "$_p" | while read -r i h; do
            [ "$h" = "$_host" ] && printf '%s\n' "$i"
          done | head -1)
    if [ -n "$_ip" ]; then printf '%s\n' "$_ip"; return 0; fi
  done
  _r=$(curl -s --connect-timeout 1 -m 2 --noproxy '*' "http://$_host.local/api/status" 2>/dev/null)
  case "$_r" in
    *"\"host\":\"$_host\""*)
      printf '%s' "$_r" | sed -n 's/.*"ip"[[:space:]]*:[[:space:]]*"\([0-9.]*\)".*/\1/p' | head -1 ;;
  esac
}

if [ "$1" = "--resolve" ]; then
  resolve "$2" "$3"
else
  scan "$1"
fi
