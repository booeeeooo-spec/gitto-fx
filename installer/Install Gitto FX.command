#!/bin/bash
# Installs the Gitto FX plugins for the current user. No admin password needed.
cd "$(dirname "$0")" || exit 1

VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
AU_DIR="$HOME/Library/Audio/Plug-Ins/Components"
mkdir -p "$VST3_DIR" "$AU_DIR"

echo "Installing Gitto FX..."
for p in VST3/*.vst3; do
    [ -e "$p" ] || continue
    rm -rf "$VST3_DIR/$(basename "$p")"
    cp -R "$p" "$VST3_DIR/" && echo "  VST3  $(basename "$p")"
done
for p in AU/*.component; do
    [ -e "$p" ] || continue
    rm -rf "$AU_DIR/$(basename "$p")"
    cp -R "$p" "$AU_DIR/" && echo "  AU    $(basename "$p")"
done

# Files downloaded from the internet are quarantined by macOS; clear that so the
# plugins are allowed to load.
xattr -dr com.apple.quarantine "$VST3_DIR"/Gitto\ FX*.vst3 "$AU_DIR"/Gitto\ FX*.component 2>/dev/null

# Make macOS re-scan Audio Units.
killall -9 AudioComponentRegistrar 2>/dev/null

echo
echo "Done. Open FL Studio and run Options > Manage plugins > Find installed plugins."
echo "You can close this window."
