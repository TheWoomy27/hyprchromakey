#!/bin/sh
# hyprland's exec drops trailing args, so the terminal gets launched from here instead
exec kitty --config NONE -o font_size=36 -o window_padding_width=20 -o background=#1e1e2e -o foreground=#ffffff -- "$(dirname "$0")/show.sh"
