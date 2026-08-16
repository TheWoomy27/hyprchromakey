-- Example hyprchromakey setup for a Lua config.
-- Verified against hyprland 0.56.2; see README.md for what each option does.


hl.plugin.load("/path/to/hyprchromakey/out/hyprchromakey.so") -- not needed if installed via hyprpm

hl.config { plugin = { hyprchromakey = {
    enabled = true,

    similarity = 0.06,
    smoothness = 0.02,
    opacity    = 0.0,
    match      = "rgb",
    min_alpha  = 0.99,

    default_windows = "off",
    default_layers  = "off",

    force_translucent = true,

    keys = table.concat({
        "rgb(1e1e2e)",
        "color rgb(181825), similarity 0.04",
        "profile term, color rgb(11111b), similarity 0.05, smoothness 0.03",
        "profile term, color rgb(1e1e2e), opacity 0.35",
    }, "; "),
}}}

hl.window_rule({ match = { class = "^(kitty)$" },        ["plugin:chromakey"] = "1" })
hl.window_rule({ match = { class = "^(Alacritty)$" },    ["plugin:chromakey"] = "term" })
hl.window_rule({ match = { class = "^(mpv|imv)$" },      ["plugin:chromakey"] = "0" })
hl.layer_rule({  match = { namespace = "^(waybar)$" },   ["plugin:chromakey"] = "1" })

hl.bind("SUPER + T",         hl.dsp.exec_cmd("hyprctl dispatch chromakey:toggle"))
hl.bind("SUPER + SHIFT + T", hl.dsp.exec_cmd("hyprctl dispatch chromakey:set term"))
