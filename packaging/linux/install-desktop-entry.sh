#!/bin/sh
# Adds SST Rematch to the desktop's application menu for this user (run it again after moving the folder). Remove it with: ./install-desktop-entry.sh --remove
here=$(dirname "$(readlink -f "$0")")
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
icons="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"

if [ "$1" = "--remove" ]; then
  rm -f "$apps/sst_rematch.desktop" "$icons/sst_rematch.png"
  echo "Removed the menu entry."
  exit 0
fi

mkdir -p "$apps" "$icons"
cp "$here/sst_rematch.png" "$icons/sst_rematch.png"
cat > "$apps/sst_rematch.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=SST Rematch
Comment=Sega Superstars Tennis, recompiled
Exec="$here/sst_rematch.sh"
Path=$here
Icon=sst_rematch
Terminal=false
Categories=Game;SportsGame;
DESKTOP
chmod +x "$apps/sst_rematch.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$apps" >/dev/null 2>&1
echo "Added SST Rematch to the application menu ($apps/sst_rematch.desktop)."