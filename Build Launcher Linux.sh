#!/usr/bin/env bash
set -euo pipefail
launcher_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
launcher_python=python3
for candidate in "$launcher_root/deps/venv/bin/python3" "$launcher_root/.venv/bin/python3"; do
    if [ -x "$candidate" ]; then launcher_python="$candidate"; break; fi
done
exec "$launcher_python" "$launcher_root/tools/build_launcher.py" --rid linux-x64 "$@"
