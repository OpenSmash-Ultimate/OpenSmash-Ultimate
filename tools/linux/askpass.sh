#!/usr/bin/env bash
# sudo -A's password dialog (SUDO_ASKPASS) for the OpenSmash Launcher's setup, which runs with no terminal: zenity
# (GNOME and most desktops), kdialog (KDE: SteamOS's and Bazzite's Desktop Mode), or ssh-askpass. Prints the
# password for sudo; with none of them, or Cancel, prints nothing and fails (sudo then stops, and the launcher's log
# says to run  bash install-windowsmash.sh  in a terminal instead).
prompt="${1:-OpenSmash setup: your password, to install the packages the build needs}"
if command -v zenity >/dev/null 2>&1; then
    exec zenity --password --title="OpenSmash setup" 2>/dev/null
elif command -v kdialog >/dev/null 2>&1; then
    exec kdialog --title "OpenSmash setup" --password "$prompt" 2>/dev/null
fi
for p in ssh-askpass ksshaskpass lxqt-openssh-askpass /usr/libexec/openssh/gnome-ssh-askpass /usr/lib/ssh/ssh-askpass; do
    if command -v "$p" >/dev/null 2>&1; then exec "$p" "$prompt"; fi
done
echo "No password dialog (zenity, kdialog or ssh-askpass) is installed: run  bash install-windowsmash.sh  in a terminal." >&2
exit 1
