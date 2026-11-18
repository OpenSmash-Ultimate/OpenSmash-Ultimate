#!/bin/bash
# Builds the OpenSmash launcher (OpenSmash.Avalonia.app, beside this file) from this folder's sources with the .NET 8
# SDK or newer. Without the SDK, tools/launcher_fetch.py takes the newest release's build instead.
# (From #345, Unify the OpenSmash launcher across Windows, Linux and macOS.)
set -euo pipefail
launcher_root="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
launcher_python=python3
for candidate in "$launcher_root/deps/venv/bin/python3" "$launcher_root/.venv/bin/python3"; do
    if [ -x "$candidate" ]; then launcher_python="$candidate"; break; fi
done
rid=osx-arm64
[ "$(uname -m)" = x86_64 ] && rid=osx-x64
exec "$launcher_python" "$launcher_root/tools/build_avalonia.py" --rid "$rid" "$@"
