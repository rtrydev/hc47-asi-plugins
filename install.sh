#!/bin/bash
# Install (or uninstall with -u) the HC47 plugins into the game.
# Works on mac (CrossOver bottle) and on Windows under Git Bash / MSYS2.
set -e
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        DEFAULT_GAME="/c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47";;
    *)
        DEFAULT_GAME="$HOME/Library/Application Support/CrossOver/Bottles/Steam/drive_c/Program Files (x86)/Steam/steamapps/common/Hitman Codename 47";;
esac
GAME="${HC47_GAME_DIR:-$DEFAULT_GAME}"
HERE="$(cd "$(dirname "$0")" && pwd)"

# Artifacts of the pre-snake_case layouts: the four pre-merge ASIs (now
# folded into hc47_tweaks.asi) and the CamelCase names of the current
# plugins. Always removed so a stale copy cannot double-patch alongside
# the current plugins (the loader picks up every *.asi in scripts/).
remove_legacy() {
    for p in HudScale HudExtras Widescreen FrameLimit Tweaks ReducedX87; do
        rm -f "$GAME/scripts/HC47$p.asi" "$GAME/scripts/HC47$p.log"
    done
    rm -rf "$GAME/scripts/HC47ReducedX87"
    rm -f "$GAME/scripts/HC47AsiLoader.log"
}

if [ "$1" = "-u" ]; then
    rm -f "$GAME/dsound.dll"
    rm -f "$GAME/scripts/hc47_asi_loader.log"
    rm -f "$GAME/scripts/hc47_reduced_x87.asi"
    rm -rf "$GAME/scripts/hc47_reduced_x87"
    rm -f "$GAME/scripts/hc47_reduced_x87.log"
    rm -f "$GAME/scripts/hc47_tweaks.asi"
    rm -f "$GAME/scripts/hc47_tweaks.log"
    remove_legacy
    # the plugins' .ini files are user config; left in place on purpose
    echo "uninstalled"
    exit 0
fi

[ -f "$HERE/dist/hc47_reduced_x87.asi" ] || { echo "build first: (cd runtime && make)"; exit 1; }
[ -f "$HERE/dist/HitmanDlc.dlc.x87" ] || { echo "generate first: python3 tools/translate.py HitmanDlc.dlc"; exit 1; }

remove_legacy

mkdir -p "$GAME/scripts/hc47_reduced_x87"
cp "$HERE/dist/hc47_reduced_x87.asi" "$GAME/scripts/"
cp "$HERE"/dist/*.x87 "$GAME/scripts/hc47_reduced_x87/"

# Combined tweaks plugin: widescreen + HUD scale + HUD extras + frame limit
if [ -f "$HERE/dist/hc47_tweaks.asi" ]; then
    cp "$HERE/dist/hc47_tweaks.asi" "$GAME/scripts/"
    if [ ! -f "$GAME/scripts/hc47_tweaks.ini" ]; then
        # migrate the config of the previous layouts when present: the
        # CamelCase combined ini, else the per-plugin inis (each already
        # carries its [Section] header); sections and keys are unchanged
        migrated=0
        if [ -f "$GAME/scripts/HC47Tweaks.ini" ]; then
            mv "$GAME/scripts/HC47Tweaks.ini" "$GAME/scripts/hc47_tweaks.ini"
            migrated=1
        else
            for p in Widescreen HudScale HudExtras FrameLimit; do
                if [ -f "$GAME/scripts/HC47$p.ini" ]; then
                    cat "$GAME/scripts/HC47$p.ini" >> "$GAME/scripts/hc47_tweaks.ini"
                    printf '\n' >> "$GAME/scripts/hc47_tweaks.ini"
                    migrated=1
                fi
            done
        fi
        if [ "$migrated" = 1 ]; then
            echo "migrated existing plugin config into scripts/hc47_tweaks.ini"
        else
            cat > "$GAME/scripts/hc47_tweaks.ini" <<'EOF'
[Widescreen]
Enabled=1
FOVFactor=1.0
DrawDistanceFactor=1.0
Borderless=-1
PreserveAspectRatio=1
ModernResolutionList=1

[HudScale]
Scale=2.0
SharpText=1

[HudExtras]
CrosshairScale=0.5
ShowTimer=1
ShowFPS=1
TextX=10
TextY=100
LineGap=16

[FrameLimit]
FpsCap=60
EOF
        fi
    fi
fi

# ASI loader: dsound.dll proxy in the game root (the name is load-bearing
# — the game's sound stack imports dsound by ordinal — so it is exempt
# from the snake_case artifact naming)
if [ -f "$HERE/dist/dsound.dll" ]; then
    cp "$HERE/dist/dsound.dll" "$GAME/dsound.dll"
fi

echo "installed to $GAME/scripts"
echo "logs will appear at: $GAME/scripts/hc47_reduced_x87.log and hc47_tweaks.log"
echo "tweaks config: $GAME/scripts/hc47_tweaks.ini"
