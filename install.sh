#!/bin/bash
# Install (or uninstall with -u) HC47 Reduced-Precision x87 into the game.
set -e
GAME="${HC47_GAME_DIR:-/Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = "-u" ]; then
    rm -f "$GAME/scripts/HC47ReducedX87.asi"
    rm -rf "$GAME/scripts/HC47ReducedX87"
    rm -f "$GAME/scripts/HC47ReducedX87.log"
    echo "uninstalled"
    exit 0
fi

[ -f "$HERE/dist/HC47ReducedX87.asi" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -f "$HERE/dist/HitmanDlc.dlc.x87" ] || { echo "generate first: python3 tools/translate.py HitmanDlc.dlc"; exit 1; }

mkdir -p "$GAME/scripts/HC47ReducedX87"
cp "$HERE/dist/HC47ReducedX87.asi" "$GAME/scripts/"
cp "$HERE"/dist/*.x87 "$GAME/scripts/HC47ReducedX87/"
echo "installed to $GAME/scripts"
echo "log will appear at: $GAME/scripts/HC47ReducedX87.log"
