#!/bin/bash
# Builds the game from your own files in one go (as launchers\Build WindowSmash.bat): the setup
# guide's steps from the Python packages to the build itself, one after the other. It asks where your
# game files, keys and firmware are, and stops with a message at the first step that fails. A step
# that is already done is skipped, so running it again carries on where it stopped. Needs what
# "tools/mac/install.sh" set up (Apple's Command Line Tools, deps/venv, deps/SDL3).
# tools/build_game.py does the work; options such as --jobs 4 are passed on to it. (tools/mac/launch.sh
# starts it.)
exec /bin/bash "$(cd "$(dirname "$0")/.." && pwd -P)/tools/mac/launch.sh" build "$@"
