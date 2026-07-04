#!/bin/bash
# Install (or uninstall with -u) the HC47 plugins into the game.
# Works on mac (CrossOver bottle) and on Windows under Git Bash / MSYS2.
set -e
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        DEFAULT_GAME="/c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47";;
    *)
        DEFAULT_GAME="/Users/rtry/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47";;
esac
GAME="${HC47_GAME_DIR:-$DEFAULT_GAME}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$1" = "-u" ]; then
    rm -f "$GAME/dsound.dll"
    rm -f "$GAME/scripts/HC47AsiLoader.log"
    rm -f "$GAME/scripts/HC47ReducedX87.asi"
    rm -rf "$GAME/scripts/HC47ReducedX87"
    rm -f "$GAME/scripts/HC47ReducedX87.log"
    rm -f "$GAME/scripts/HC47HudScale.asi"
    rm -f "$GAME/scripts/HC47HudScale.log"
    rm -f "$GAME/scripts/HC47HudExtras.asi"
    rm -f "$GAME/scripts/HC47HudExtras.log"
    rm -f "$GAME/scripts/HC47Widescreen.asi"
    rm -f "$GAME/scripts/HC47Widescreen.log"
    rm -f "$GAME/scripts/HC47FrameLimit.asi"
    rm -f "$GAME/scripts/HC47FrameLimit.log"
    # the plugins' .ini files are user config; left in place on purpose
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

# HUD extras: crosshair shrink + mission timer + FPS overlay
if [ -f "$HERE/dist/HC47HudExtras.asi" ]; then
    cp "$HERE/dist/HC47HudExtras.asi" "$GAME/scripts/"
    if [ ! -f "$GAME/scripts/HC47HudExtras.ini" ]; then
        printf '[HudExtras]\nCrosshairScale=0.5\nShowTimer=1\nShowFPS=1\nTextX=10\nTextY=100\nLineGap=16\n' > "$GAME/scripts/HC47HudExtras.ini"
    fi
fi

# Widescreen: resolution passthrough + aspect-correct FOV
if [ -f "$HERE/dist/HC47Widescreen.asi" ]; then
    cp "$HERE/dist/HC47Widescreen.asi" "$GAME/scripts/"
    if [ ! -f "$GAME/scripts/HC47Widescreen.ini" ]; then
        printf '[Widescreen]\nEnabled=1\nFOVFactor=1.0\nDrawDistanceFactor=1.0\n' > "$GAME/scripts/HC47Widescreen.ini"
    fi
fi

# ASI loader: dsound.dll proxy in the game root
if [ -f "$HERE/dist/dsound.dll" ]; then
    cp "$HERE/dist/dsound.dll" "$GAME/dsound.dll"
fi

# Frame limiter: configurable FPS cap
if [ -f "$HERE/dist/HC47FrameLimit.asi" ]; then
    cp "$HERE/dist/HC47FrameLimit.asi" "$GAME/scripts/"
    if [ ! -f "$GAME/scripts/HC47FrameLimit.ini" ]; then
        printf '[FrameLimit]\nFpsCap=60\n' > "$GAME/scripts/HC47FrameLimit.ini"
    fi
fi

echo "installed to $GAME/scripts"
echo "log will appear at: $GAME/scripts/HC47ReducedX87.log"
echo "HUD scale config: $GAME/scripts/HC47HudScale.ini (Scale=1.0 disables)"
