#!/usr/bin/env bash
# The Linux setup's old name, kept for older launchers and habits: it runs install-opensmash.sh, next to it,
# with the same arguments, and exits with its exit code.
exec bash "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/install-opensmash.sh" "$@"
