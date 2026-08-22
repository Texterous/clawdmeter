#!/bin/sh
# Clawdmeter agent bootstrap (macOS / Linux).
#
# Served by the device itself at http://<device>/agent/install.sh, because the
# machine that needs this may be joined to the device's setup access point with
# no route to the internet. In that case it says so and tells you what to do,
# rather than hanging on a download that cannot succeed.
#
#   curl -fsSL http://clawdmeter-XXXX.local/agent/install.sh | sh
#
# Optional: CLAWDMETER_DEVICE=<host> to skip discovery, VERSION=<tag> to pin.

set -eu

REPO="Texterous/clawdmeter"
VERSION="${VERSION:-latest}"
DEST_DIR="${CLAWDMETER_PREFIX:-$HOME/.local/bin}"
DEST="$DEST_DIR/clawdmeter-agent"

say() { printf '  %s\n' "$1"; }

printf '\nClawdmeter agent installer\n\n'

# --- Which build? ----------------------------------------------------------
os=$(uname -s)
arch=$(uname -m)
case "$os" in
  Darwin) os_tag=macos ;;
  Linux)  os_tag=linux ;;
  *) printf 'Unsupported OS: %s\n' "$os" >&2
     say "Build from source: https://github.com/$REPO"
     exit 1 ;;
esac
case "$arch" in
  x86_64|amd64) arch_tag=x64 ;;
  arm64|aarch64) arch_tag=arm64 ;;
  *) printf 'Unsupported architecture: %s\n' "$arch" >&2
     say "Build from source: https://github.com/$REPO"
     exit 1 ;;
esac
ASSET="clawdmeter-agent-${os_tag}-${arch_tag}"

# --- Is there a route to the internet at all? ------------------------------
# The common failure is running this while still joined to Clawdmeter-Setup,
# which has no uplink. Detect it up front and be explicit.
if ! curl -fsS --head --max-time 8 https://api.github.com >/dev/null 2>&1; then
  printf 'No route to github.com.\n'
  say 'If you are joined to the Clawdmeter-Setup network, that is expected:'
  say 'it has no internet uplink. Finish setting up WiFi on the device, rejoin'
  say 'your normal network, then run this again.'
  say ''
  say "Or download it by hand: https://github.com/$REPO/releases/latest"
  exit 1
fi

# --- Resolve the release asset --------------------------------------------
if [ "$VERSION" = latest ]; then
  API="https://api.github.com/repos/$REPO/releases/latest"
else
  API="https://api.github.com/repos/$REPO/releases/tags/$VERSION"
fi

# No jq dependency: pull the one browser_download_url whose line matches ASSET.
URL=$(curl -fsSL -H 'User-Agent: clawdmeter-installer' "$API" \
      | tr ',' '\n' \
      | grep 'browser_download_url' \
      | grep "$ASSET" \
      | head -1 \
      | sed -E 's/.*"(https:[^"]+)".*/\1/')

if [ -z "${URL:-}" ]; then
  printf 'Release %s has no %s.\n' "$VERSION" "$ASSET" >&2
  say "See https://github.com/$REPO/releases"
  exit 1
fi

# --- Install --------------------------------------------------------------
mkdir -p "$DEST_DIR"
say "Downloading $(basename "$URL")..."
curl -fsSL "$URL" -o "$DEST"
chmod +x "$DEST"
say "Installed to $DEST"

case ":$PATH:" in
  *":$DEST_DIR:"*) ;;
  *) say "Note: $DEST_DIR is not on your PATH." ;;
esac

# --- Start it -------------------------------------------------------------
# No CLAWDMETER_DEVICE means the agent discovers clawdmeter-*.local over mDNS,
# which is the path we want people on: no IP to look up, nothing to re-enter
# when DHCP moves the device.
printf '\n'
if [ -n "${CLAWDMETER_DEVICE:-}" ]; then
  say "Starting against $CLAWDMETER_DEVICE"
  exec "$DEST" --device "$CLAWDMETER_DEVICE"
else
  say 'Starting with mDNS discovery'
  exec "$DEST"
fi
