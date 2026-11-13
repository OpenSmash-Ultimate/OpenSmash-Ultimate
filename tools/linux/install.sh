#!/usr/bin/env bash
# WindowSmash on Linux in one go: it gets the newest WindowSmash, installs what the build needs
# from your distribution (sudo asks for your password), finds your game files, and builds the
# game (an hour to a few, the first time). Run it from a terminal in this folder:
#
#   bash tools/linux/install.sh
#
# Run it again at any time: it carries on where it stopped. Its options: bash tools/linux/install.sh --help
# Everything it says also goes to install.log. What each step does: tools/linux/install_linux.py.
{
    cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/../.." || exit 1

    # Python 3.10 or newer runs the rest
    find_python() {
        local p
        for p in python3 python3.14 python3.13 python3.12 python3.11 python3.10 python; do
            if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys; sys.exit(sys.version_info < (3, 10))' 2>/dev/null; then
                echo "$p"
                return 0
            fi
        done
        return 1
    }
    py=$(find_python)
    if [ -z "$py" ]; then
        echo "Setting up WindowSmash needs Python 3.10 or newer: installing it (sudo asks for your password)."
        if [ -e /run/ostree-booted ] && command -v rpm-ostree >/dev/null 2>&1; then   # (an image-based system)
            sudo rpm-ostree install --idempotent --allow-inactive --apply-live python3
        elif command -v apt-get >/dev/null 2>&1; then
            sudo apt-get update && sudo apt-get install -y python3 python3-venv
        elif command -v dnf >/dev/null 2>&1; then
            sudo dnf install -y python3
        elif command -v pacman >/dev/null 2>&1; then
            sudo pacman -S --needed --noconfirm python
        elif command -v zypper >/dev/null 2>&1; then
            sudo zypper --non-interactive install python3
        fi
        py=$(find_python) || { echo "Install Python 3.10 or newer, then run this again."; exit 1; }
    fi
    "$py" -u tools/linux/install_linux.py "$@" 2>&1 | tee -a install.log
    exit "${PIPESTATUS[0]}"
}
