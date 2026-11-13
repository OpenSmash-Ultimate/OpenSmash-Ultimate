#!/usr/bin/env python3
"""Internal source/package refresh; game installation is separate."""
import sys
from pathlib import Path
from types import SimpleNamespace

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "linux"))
import install_linux


def main():
    args = SimpleNamespace(no_update=False, dry_run=False)
    if "--python-only" in sys.argv:
        if sys.platform == "darwin":
            sys.path.insert(0, str(HERE / "mac"))
            import install_mac
            install_mac.step_python(args)
        else:
            install_linux.step_python(args)
    else:
        install_linux.step_update(args)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Source update stopped: {error}", file=sys.stderr)
        sys.exit(1)
