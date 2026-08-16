#!/usr/bin/env bash
# End-to-end check: starts a throwaway nested Hyprland (its own compositor, in a window on your
# desktop), loads the plugin into it, and screenshots the result. Nothing touches your session -
# if the plugin misbehaves, only the nested instance is affected.
#
#   ./run-nested.sh with-plugin    out/    # keyed
#   ./run-nested.sh without-plugin out/    # reference shot
#
# The nested session paints its background bright green and runs a terminal with a #1e1e2e
# background, so a working chromakey turns the terminal green while its text stays white.
# Needs: kitty, grim. Optional: magick, for the pixel checks below.
set -u

MODE="${1:-with-plugin}"
OUT="${2:-$(mktemp -d)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
PLUGIN="$REPO/out/hyprchromakey.so"

if [ "$MODE" = "with-plugin" ] && [ ! -f "$PLUGIN" ]; then
    echo "build the plugin first: make -C $REPO"
    exit 1
fi

mkdir -p "$OUT"
CONF="$OUT/hypr.conf"
sed -e "s|PLUGIN_PATH|$PLUGIN|" -e "s|LAUNCHER|$HERE/launch-kitty.sh|" "$HERE/hc-test.conf" > "$CONF"
[ "$MODE" = "with-plugin" ] || sed -i '/^plugin = /d' "$CONF"

HOST_SOCKETS=$(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock$')
HOST_INSTANCES=$(ls -d "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null)

Hyprland --config "$CONF" > "$OUT/hyprland.log" 2>&1 &
HYPR_PID=$!
cleanup() { kill -TERM $HYPR_PID 2>/dev/null; sleep 1; kill -KILL $HYPR_PID 2>/dev/null; }
trap cleanup EXIT

# the nested instance picks the next free wayland socket; find whichever one is new
NESTED=""
for _ in $(seq 1 60); do
    for s in $(ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | grep -v '\.lock$'); do
        echo "$HOST_SOCKETS" | grep -qx "$s" || { NESTED="$(basename "$s")"; break 2; }
    done
    sleep 0.5
done

if [ -z "$NESTED" ]; then
    echo "the nested compositor never came up:"
    tail -30 "$OUT/hyprland.log"
    exit 1
fi
export WAYLAND_DISPLAY="$NESTED"

HIS=""
for _ in $(seq 1 20); do
    for d in $(ls -d "$XDG_RUNTIME_DIR"/hypr/*/ 2>/dev/null); do
        echo "$HOST_INSTANCES" | grep -qx "$d" || { HIS="$(basename "${d%/}")"; break 2; }
    done
    sleep 0.5
done
export HYPRLAND_INSTANCE_SIGNATURE="$HIS"

sleep 7 # let the terminal map and paint

{
    hyprctl plugin list
    hyprctl chromakey
} > "$OUT/status.txt" 2>&1

grim "$OUT/screen.png" 2>> "$OUT/hyprland.log"
cp "$XDG_RUNTIME_DIR/hypr/$HIS/hyprland.log" "$OUT/inner.log" 2>/dev/null

echo "screenshot: $OUT/screen.png"
cat "$OUT/status.txt"

if command -v magick > /dev/null; then
    W=$(magick identify -format '%w' "$OUT/screen.png")
    H=$(magick identify -format '%h' "$OUT/screen.png")
    BG=$(magick "$OUT/screen.png" -format "%[pixel:p{$((W / 2)),$((H * 3 / 4))}]" info:)
    echo
    echo "terminal background reads $BG"
    case "$MODE:$BG" in
        with-plugin:srgb\(0,255,0\))    echo "PASS - the background keyed out" ;;
        without-plugin:srgb\(30,30,46\)) echo "PASS - reference shot looks right" ;;
        *) echo "UNEXPECTED - look at $OUT/screen.png" ;;
    esac
fi
