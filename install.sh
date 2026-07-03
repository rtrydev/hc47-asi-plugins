#!/bin/bash
# Install (or uninstall with -u) HC47 Reduced-Precision x87 into the game.
set -e
GAME="${HC47_GAME_DIR:-/Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = "-u" ]; then
    rm -f "$GAME/scripts/HC47ReducedX87.asi"
    rm -rf "$GAME/scripts/HC47ReducedX87"
    rm -f "$GAME/scripts/HC47ReducedX87.log"
    rm -f "$GAME/scripts/HC47HudScale.asi"
    rm -f "$GAME/scripts/HC47HudScale.log"
    # HC47HudScale.ini is user config; left in place on purpose
    echo "uninstalled"
    exit 0
fi

[ -f "$HERE/dist/HC47ReducedX87.asi" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -f "$HERE/dist/HitmanDlc.dlc.x87" ] || { echo "generate first: python3 tools/translate.py HitmanDlc.dlc"; exit 1; }

mkdir -p "$GAME/scripts/HC47ReducedX87"
cp "$HERE/dist/HC47ReducedX87.asi" "$GAME/scripts/"
cp "$HERE"/dist/*.x87 "$GAME/scripts/HC47ReducedX87/"

# HUD scale (independent of the x87 patch; works in any window mode)
if [ -f "$HERE/dist/HC47HudScale.asi" ]; then
    cp "$HERE/dist/HC47HudScale.asi" "$GAME/scripts/"
    if [ ! -f "$GAME/scripts/HC47HudScale.ini" ]; then
        printf '[HudScale]\nScale=2.0\n' > "$GAME/scripts/HC47HudScale.ini"
    fi
fi

echo "installed to $GAME/scripts"
echo "log will appear at: $GAME/scripts/HC47ReducedX87.log"
echo "HUD scale config: $GAME/scripts/HC47HudScale.ini (Scale=1.0 disables)"
