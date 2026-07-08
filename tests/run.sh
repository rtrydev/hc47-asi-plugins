#!/bin/bash
# Run the differential tester inside the CrossOver Steam bottle.
set -e
GAME="${HC47_GAME_DIR:-$HOME/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47}"
GAME_WIN='C:\Program Files (x86)\Steam\steamapps\common\Hitman Codename 47'
WINE="/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine"
HERE="$(cd "$(dirname "$0")" && pwd)"

make -C "$HERE"
cp "$HERE/difftest.exe" "$HERE/../dist/HitmanDlc.dlc.x87" \
   "$HERE/../dist/HitmanDlc.dlc.leaf.txt" "$GAME/"
"$WINE" --bottle Steam --workdir "$GAME_WIN" \
    --cx-app "$GAME_WIN\\difftest.exe" \
    HitmanDlc.dlc HitmanDlc.dlc.x87 HitmanDlc.dlc.leaf.txt 2>&1 \
    | grep -vE 'fixme|err:|warn:|wine:'
rm -f "$GAME/difftest.exe" "$GAME/HitmanDlc.dlc.x87" "$GAME/HitmanDlc.dlc.leaf.txt"
