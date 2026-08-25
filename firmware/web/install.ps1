# Clawdmeter agent bootstrap (Windows / PowerShell) -- WITHDRAWN, installs nothing.
#
# This used to fetch a release asset from Texterous/clawdmeter. That repository
# has never published a release -- /releases answers 200 with an empty array and
# /releases/latest answers 404 -- so the download could not succeed on any
# machine, ever. Worse, it failed through the reachability probe and reported
# "No route to github.com", which reads as a broken network and sent people off
# to debug their WiFi instead of the release that was never cut.
#
# The device does not serve this file any more either: Web.cpp no longer
# registers /agent/install.ps1. It stays in the tree only because
# tools/gen_agent.py requires a source file to read, and so that anyone who runs
# it out of a repository checkout is told the truth instead of nothing.
#
# The sender that does work is a Python daemon. The device's own web page walks
# through installing it and prints the commands with this unit's address already
# substituted -- which is the part a static script cannot do.

Write-Host ''
Write-Host 'This installer has been withdrawn -- it installs nothing.' -ForegroundColor Yellow
Write-Host 'There is no agent release to download, and there never was one.'
Write-Host ''
Write-Host 'Open the device web page and read the Clawdmeter tab. It has the five'
Write-Host 'setup steps with this unit''s address already filled in. The address is'
Write-Host 'shown on the device screen -- typically:'
Write-Host '    http://clawd-XXXX.local'
Write-Host ''
Write-Host 'The sender itself: https://github.com/giovi321/clawdmeter-daemon'
Write-Host ''
exit 1
