#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# The PianoForge plugin for Flathub's OBS Studio (com.obsproject.Studio), in one command:
#   obs/install-flatpak.sh
# builds it inside that OBS's own SDK (against its libobs), installs it where that OBS looks
# (~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/pianoforge-obs), and lets that OBS see PianoForge's
# feed (a Flatpak keeps a /dev/shm of its own: --device=shm gives it the system's). Restart OBS afterwards.
set -e
cd "$(dirname "$0")/.."

app=com.obsproject.Studio
if ! flatpak info "$app" > /dev/null 2>&1; then
    echo "OBS Studio is not installed from Flathub (flatpak install flathub $app)." >&2
    exit 1
fi
sdk=$(flatpak info --show-sdk "$app")
if ! flatpak info "$sdk" > /dev/null 2>&1; then
    echo "OBS's SDK is needed to build the plugin: flatpak install flathub $sdk" >&2
    exit 1
fi

flatpak run --devel --command=sh "$app" -c \
    "cmake -S obs -B build/obs-flatpak -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/app > /dev/null \
     && cmake --build build/obs-flatpak --target install-obs-plugin"
flatpak override --user --device=shm "$app"

echo "Done. Restart OBS, then add the sources PianoForge (audio), PianoForge keyboard and PianoForge text."
