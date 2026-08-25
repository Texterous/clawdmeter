#!/bin/sh
# clawd-find.sh — locate Clawdmeter units on the local network.
#
# Used by /clawd:setup only. The hook path never needs this: once paired, the
# address is in ~/.clawd/config. Prints one "<ip> <host>" line per unit found.

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
    'r=$(curl -s --connect-timeout 0.3 -m 1 "http://'"$_prefix"'.@/api/status" 2>/dev/null)
     case "$r" in
       *"\"fw\":\"clawdmeter\""*)
         h=$(printf "%s" "$r" | sed -n "s/.*\"host\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p")
         echo "'"$_prefix"'.@ ${h:-?}" ;;
     esac' 2>/dev/null
}

scan "$1"
