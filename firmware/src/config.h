// config.h — compile-time constants for Clawdmeter.
//
// Hardware: GeekMagic SmallTV-Ultra — ESP-12F (ESP8266), 4 MB flash,
// 1.54" 240x240 ST7789 IPS panel. Pin map and panel quirks live in Board.h.
//
// One screen, one job: show the Claude Code usage that an agent pushes to
// POST /api/usage. Everything the upstream mod carried for other jobs
// (ticker, radar, Home Assistant, WireGuard, notify overlays) is gone.
//
// Derived from giovi321/smalltv-mod (WTFPL). See ../../README.md.
#pragma once

// ---------------------------------------------------------------------------
// Firmware identity
// ---------------------------------------------------------------------------
#define FW_NAME     "clawdmeter"
#define FW_VERSION  "0.2.1"

#define REPO_URL    "https://github.com/Texterous/clawdmeter"
#define REPO_OWNER  "Texterous"
#define REPO_NAME   "clawdmeter"
#define GH_API_HOST "api.github.com"

// Release asset the GitHub self-updater pulls. The slim image has no TLS stack,
// so it cannot self-update at all — it is upgraded by uploading to /update.
#define UPDATE_ASSET "clawdmeter-ultra.bin"

// NO_TLS selects the slim build: no BearSSL, no HTTPS, no GitHub self-update.
// This is the image that has to squeeze through the stock Ultra OTA slot, and it
// needs no TLS because usage is PUSHED to the device, never fetched.
//
// Every call site that could pull BearSSL into the link must sit behind
// WITH_TLS. A single unguarded reference (even one behind a runtime `if`) drags
// the whole stack back in and costs the slim build the ~80-120 KB it exists to
// save, so treat this as load-bearing rather than cosmetic.
#ifdef NO_TLS
  #define WITH_TLS 0
#else
  #define WITH_TLS 1
#endif
#define WITH_SELFUPDATE WITH_TLS

// ---------------------------------------------------------------------------
// Branding. DEVICE_LABEL is overridable at build time so a giveaway batch can
// be stamped without touching source.
// ---------------------------------------------------------------------------
#ifndef DEVICE_LABEL
#define DEVICE_LABEL "Texterous"
#endif

// Minimum time the brand mark stays on screen at boot. It is a MINIMUM, not a
// delay: WiFi association happens underneath it and counts toward the total, so
// on a normal boot this costs nothing and only pads out a fast connect.
#ifndef SPLASH_MIN_MS
#define SPLASH_MIN_MS 3000UL
#endif
#define AGENT_URL   "https://github.com/Texterous/clawdmeter"

// ---------------------------------------------------------------------------
// Display wiring + panel quirks (SCLK/MOSI/DC/RST/CS/BL, TFT_BGR,
// TFT_BL_DEFAULT_INVERTED, HAS_LDR/LDR_PIN/ADC_MAX).
// ---------------------------------------------------------------------------
#include "Board.h"

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// Panel RAM offsets per rotation pair (Arduino_GFX: offset1 -> rotation 0/1,
// offset2 -> rotation 2/3). The ST7789(V) is a 240x320 controller but the 1.54"
// glass only wires RAM rows 0-239, leaving an 80-row dead band. With the MADCTL
// MY bit set (rotation 2/3) the scan direction reverses into that dead band, so
// the row offset must jump to 80 or the image slides 80 px off the glass.
#ifndef TFT_COL_OFFSET1
#define TFT_COL_OFFSET1 0
#endif
#ifndef TFT_ROW_OFFSET1
#define TFT_ROW_OFFSET1 0
#endif
#ifndef TFT_COL_OFFSET2
#define TFT_COL_OFFSET2 0
#endif
#ifndef TFT_ROW_OFFSET2
#define TFT_ROW_OFFSET2 80
#endif

// ---------------------------------------------------------------------------
// Limits (bound RAM on the ESP8266)
// ---------------------------------------------------------------------------
#define MAX_WIFI_NETS     4    // saved networks; strongest visible wins at boot
#define MAX_URL_LEN     200

// ---------------------------------------------------------------------------
// Network timing (Net.cpp)
// ---------------------------------------------------------------------------
// Per-candidate association budget at boot. A network the scan just saw gets the
// long window; one it did not (hidden SSID, or out of range) gets the short one.
#define WIFI_JOIN_SEEN_MS     15000UL
#define WIFI_JOIN_UNSEEN_MS    8000UL

// The AP is a waiting room: with nobody connected to the captive portal, retry
// the saved networks this often rather than sitting there until someone
// power-cycles the unit. The retry costs a scan plus one association attempt per
// visible network, so keep it well above that.
#define AP_RETRY_MS          120000UL

// While the station is down: how often to nudge the current network, and how
// long to stay down before trying the next saved one. The nudge is a full
// disconnect/connect, so it has to be slower than an association can complete.
#define STA_NUDGE_MS          30000UL
#define STA_ROTATE_MS         90000UL

// ---------------------------------------------------------------------------
// Web UI password. Off by default — the page is open on the LAN, as upstream
// has it. Digest auth, so the password never crosses the wire in clear.
// ---------------------------------------------------------------------------
#define MAX_AUTH_USER_LEN 32
#define MAX_AUTH_PASS_LEN 64
#define DEFAULT_AUTH_USER "admin"
#define AUTH_REALM        "Clawdmeter"

// ---------------------------------------------------------------------------
// Display modes. Registered in main.cpp's kModes[]; settings.mode picks one.
//   usage    — the 5h/7d meters and the mascot
//   sessions — the session board: what every live Claude session is doing
// ---------------------------------------------------------------------------
#define MODE_USAGE    1
#define MODE_SESSIONS 2
#define DEFAULT_MODE  MODE_USAGE

// Session board. The daemon caps `sess` at six rows to match what fits on the
// panel; MAX_SESSION_ROWS is the array that receives them.
#define MAX_SESSION_ROWS   6
#define SESSION_NAME_LEN  12   // 12 chars at text size 2 = 144 px, the row width

// Once usage stops arriving for this long (laptop asleep, agent stopped,
// network down) the screen leaves the stats for the idle mascot animation.
// The effective timeout also scales with the poll period (see UsageMode).
#define USAGE_STALE_GRACE_MS  20000UL

// ---------------------------------------------------------------------------
// Provisioning. Baking event WiFi into a batch image means a freshly flashed
// unit rejoins the venue network by itself, so nobody touches a setup screen
// on event day. Both must be set for the credential to be seeded.
//   -DPROVISION_SSID='"HackathonWiFi"' -DPROVISION_PASS='"..."'
// ---------------------------------------------------------------------------
// provision_local.h is gitignored and written by provision/flash.ps1. Using a
// header rather than -D flags is deliberate: an SSID containing a space cannot
// survive PlatformIO's flag splitting, and a credential in platformio.ini would
// end up committed.
#if defined(__has_include)
  #if __has_include("provision_local.h")
    #include "provision_local.h"
  #endif
#endif

#if defined(PROVISION_SSID) && defined(PROVISION_PASS)
  #define HAS_PROVISION 1
#else
  #define HAS_PROVISION 0
#endif

// ---------------------------------------------------------------------------
// Defaults (first boot / factory reset)
// ---------------------------------------------------------------------------
// Both of these get the device's chip suffix appended (Settings::setDefaults),
// so a batch of units never puts identical names in the air.
#define DEFAULT_AP_SSID      "Clawdmeter-Setup"
#define DEFAULT_AP_PASS      ""              // empty => open AP
#define DEFAULT_HOSTNAME     "clawdmeter"
#define DEFAULT_BRIGHTNESS    90             // 0..100 %
#define DEFAULT_HTTP_TIMEOUT  8000           // ms per request
#define DEFAULT_USAGE_POLL_SEC 30            // only used if a pull URL is set

// --- Panel colour correction (device-wide) ---
// Panels differ between units: white balance drifts and a few controllers have
// red and blue swapped. AUTO keeps Board.h's TFT_BGR default.
#define COLOR_ORDER_AUTO   0
#define COLOR_ORDER_RGB    1
#define COLOR_ORDER_BGR    2
#define DEFAULT_COLOR_ORDER  COLOR_ORDER_AUTO
#define DEFAULT_COLOR_INVERT false
#define DEFAULT_COLOR_GAIN   100     // percent per channel; 50..150 accepted
#define MIN_COLOR_GAIN        50
#define MAX_COLOR_GAIN       150

// --- Clock / night mode (device-wide) ---
#define NTP_SERVER1             "pool.ntp.org"
#define NTP_SERVER2             "time.nist.gov"
#define DEFAULT_TZ_NAME         ""        // IANA display name; empty = UTC
#define DEFAULT_TZ_POSIX        "UTC0"    // POSIX TZ rule fed to SNTP
#define DEFAULT_NIGHT_ENABLED   false
#define DEFAULT_NIGHT_START_MIN 1320      // 22:00
#define DEFAULT_NIGHT_END_MIN   420       // 07:00
#define DEFAULT_NIGHT_LEVEL     0         // 0..100, 0 = backlight fully off

// Night-mode NTP trust: only ENTER night mode when the clock was confirmed by a
// successful NTP sync within NIGHT_NTP_TRUST_MS, else assume the clock may be
// wrong and keep the screen on. While inside the window but unconfirmed, re-arm
// SNTP every NIGHT_NTP_RESYNC_MS until a sync lands or the window ends.
#define NIGHT_NTP_TRUST_MS      300000UL
#define NIGHT_NTP_RESYNC_MS      30000UL
