// BearSslTuning.cpp — pin the ESP8266's BearSSL elliptic-curve support to P-256.
//
// Only the full image compiles this: the slim (NO_TLS) build has no TLS stack to
// tune, and pulling in a bearssl header there would defeat the point.
//
// Full BearSSL by default advertises x25519 + P-256/384/521 and lets the server
// choose; x25519 in particular is heavy on this chip. Defining br_ec_get_default()
// here overrides the library's version at link time (our object satisfies the
// reference before ec_default.o is pulled from the archive), forcing P-256 only
// (the prebuilt lib ships br_ec_p256_m15). That avoids the much heavier x25519 a
// server could otherwise pick, and drops the unused curve code from flash.
//
// The only TLS user left is the optional GitHub self-update, which talks to
// api.github.com and its asset CDN.
#include "config.h"

#if WITH_TLS && defined(ESP8266) && !defined(BEARSSL_SSL_BASIC)
#include <bearssl/bearssl_ec.h>

extern "C" const br_ec_impl *br_ec_get_default(void) {
  return &br_ec_p256_m15;
}
#endif
