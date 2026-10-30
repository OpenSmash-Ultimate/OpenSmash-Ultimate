#!/bin/bash
# Imports your own Miis into miis/, where the game reads them (tools/miis.py, as Import Miis.bat):
# the Miis of every emulator found on this Mac (Ryujinx, yuzu, Eden, Citron, ...), then any Mii
# Studio links you paste. Mii files given as arguments are imported too. Restart the game
# afterwards. (tools/mac/launch.sh does the work.)
exec /bin/bash "$(cd "$(dirname "$0")/.." && pwd -P)/tools/mac/launch.sh" miis "$@"
