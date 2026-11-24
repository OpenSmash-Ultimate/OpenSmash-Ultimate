#!/bin/bash
# The Mac setup's old name, kept for older launchers and habits: it runs "Install OpenSmash.command", next
# to it, with the same arguments, and exits with its exit code.
exec /bin/bash "$(cd "$(dirname "$0")" && pwd -P)/Install OpenSmash.command" "$@"
