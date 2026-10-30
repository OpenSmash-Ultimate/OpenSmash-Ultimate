#!/usr/bin/env python3
"""locked.py <command...>: run a command holding the shared test lock (/tmp/windowsmash-test.lock,
bench.py's), and never while the user's own copy of the game runs (builds and probe boots)."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench  # noqa: E402

bench.take_lock(f"locked.py {' '.join(sys.argv[1:])[:80]} {os.getpid()}")
try:
    sys.exit(subprocess.call(sys.argv[1:]))
finally:
    bench.drop_lock()
