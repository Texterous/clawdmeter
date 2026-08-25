#!/bin/sh
# Clawdmeter agent bootstrap (macOS / Linux) -- WITHDRAWN, installs nothing.
#
# This used to fetch a release asset from Texterous/clawdmeter. That repository
# has never published a release -- /releases answers 200 with an empty array and
# /releases/latest answers 404 -- so the download could not succeed on any
# machine, ever. The old code made that worse in two ways: the reachability
# probe reported "No route to github.com", which reads as a broken network, and
# the asset lookup piped `curl -fsSL` into tr/grep/sed with no `set -o pipefail`,
# so a 404 from the API surfaced as the far less accurate "Release latest has no
# clawdmeter-agent-linux-x64" rather than as the failed request it was.
#
# The device does not serve this file any more either: Web.cpp no longer
# registers /agent/install.sh. It stays in the tree only because
# tools/gen_agent.py requires a source file to read, and so that anyone who runs
# it out of a repository checkout is told the truth instead of nothing.
#
# The sender that does work is a Python daemon. The device's own web page walks
# through installing it and prints the commands with this unit's address already
# substituted -- which is the part a static script cannot do.

printf '\nThis installer has been withdrawn -- it installs nothing.\n'
printf 'There is no agent release to download, and there never was one.\n\n'
printf 'Open the device web page and read the Clawdmeter tab. It has the five\n'
printf 'setup steps with this unit'\''s address already filled in. The address is\n'
printf 'shown on the device screen -- typically:\n'
printf '    http://clawd-XXXX.local\n\n'
printf 'The sender itself: https://github.com/giovi321/clawdmeter-daemon\n\n'
exit 1
