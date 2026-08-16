#!/bin/sh
# paint the whole terminal #1e1e2e with white text, no shell config involved
printf '\033[48;2;30;30;46m\033[38;2;255;255;255m\033[2J\033[H'
printf '\n\n   HYPRCHROMA\n\n   this text must stay opaque\n\n   the background must key out\n'
sleep 600
