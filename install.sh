#!/bin/sh
# curl -fsSL https://raw.githubusercontent.com/darealhoks/whimsy/main/install.sh | sh
set -eu

REPO=${WHIMSY_REPO:-https://github.com/darealhoks/whimsy.git}
SRC=${WHIMSY_SRC:-$HOME/.local/share/whimsy/src}
BIN=${WHIMSY_BIN:-$HOME/.local/bin}
APPS=$HOME/.local/share/applications

for t in git make cc pkg-config; do
  command -v "$t" >/dev/null || { echo "missing: $t"; exit 1; }
done

if [ -d "$SRC/.git" ]; then git -C "$SRC" pull --ff-only
else mkdir -p "$(dirname "$SRC")"; git clone --depth 1 "$REPO" "$SRC"; fi

make -C "$SRC" MODE=release -j"$(nproc 2>/dev/null || echo 2)"

mkdir -p "$BIN" "$APPS"
for b in whimsy whimsyd; do
  [ -f "$SRC/build/release/$b" ] && install -m755 "$SRC/build/release/$b" "$BIN/$b"
done

if [ -f "$BIN/whimsy" ]; then
  cat > "$APPS/whimsy.desktop" <<DESK
[Desktop Entry]
Type=Application
Name=whimsy
Comment=Invite-only e2e messenger
Exec=$BIN/whimsy
Terminal=false
Categories=Network;InstantMessaging;
DESK
  command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" || true
else
  echo "no gui built (needs sdl3 + freetype2); installed whimsyd only"
fi

case ":$PATH:" in *:"$BIN":*) ;; *) echo "add $BIN to PATH";; esac
echo "installed to $BIN"
