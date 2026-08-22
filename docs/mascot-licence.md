# Mascot artwork: licence status

**Status: unresolved. Settle this before handing out hardware.**

The pixel-creature animation frames in
[`firmware/src/meter/mascot_frames.h`](../firmware/src/meter/mascot_frames.h) —
about 32 KB of PROGMEM, five animations — originate from **ClaudePix**
(https://claudepix.vercel.app), reaching this project via
`giovi321/smalltv-mod`, which credits it.

The problem: the apparent original repository (`amaancoderx/claudepix`) publishes
**no licence file**. Under default copyright that means all rights reserved — not
"free to use". An Apache-2.0 reimplementation exists
(`igouss/claudepix-rs`), which may or may not cover the frame data itself.

This matters more here than in a hobby fork, on two counts:

1. This is a **public repository under an organisation's name**.
2. The output is **physical objects handed to other people** at a branded event.

## Options, in order of preference

1. **Ask the author.** A short message asking whether the frames may be used and
   redistributed, ideally answered with a licence file on the repo. Cheapest and
   cleanest.
2. **Use the Apache-2.0 port's data,** if its frame data is genuinely covered by
   that licence and is the same artwork. Verify rather than assume — an Apache-2.0
   header on a renderer does not automatically license art it consumes.
3. **Draw our own.** [`tools/gen_mascot.py`](../firmware/tools/gen_mascot.py) takes
   any sprite sheet, so a Texterous-drawn creature is a drop-in replacement. It
   also frees ~32 KB of flash, which is real margin on this chip, and puts our own
   character on the screen instead of someone else's.

Option 3 is the fallback that needs no permission from anybody, and the timeline
has room for it.

## Separately: the name

"Claude" and "Clawd" are Anthropic's marks. Using them descriptively — this device
shows your Claude Code usage — is ordinary nominative use and fine for an event
giveaway. It would deserve a second look if these were ever sold, or if the
branding implied endorsement. The README already states no affiliation.
