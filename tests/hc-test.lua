-- Config for the nested end-to-end run. run-nested.sh fills in PLUGIN_PATH and LAUNCHER.

hl.monitor({ output = "", mode = "1280x720@60", position = "0x0", scale = 1 })

hl.config {
    ecosystem = { no_donation_nag = true, no_update_news = true },
    misc = {
        disable_hyprland_logo    = true,
        disable_splash_rendering = true,
        force_default_wallpaper  = 0,
        -- a loud green behind everything, so anything keyed out is unmistakable
        background_color         = 0xff00ff00,
        disable_autoreload       = true,
    },
    animations = { enabled = false },
    -- the plugin logs what it compiles and rejects; worth having when this test fails
    debug      = { disable_logs = false },
    decoration = { rounding = 0, blur = { enabled = false }, shadow = { enabled = false } },
    general    = { border_size = 2, gaps_in = 0, gaps_out = 0 },
}

hl.plugin.load("PLUGIN_PATH")

hl.config { plugin = { hyprchromakey = {
    enabled    = true,
    similarity = 0.06,
    smoothness = 0.02,
    -- kitty's background in the test session, plus a profile that must parse but stays unused
    keys       = "rgb(1e1e2e); profile alt, color rgb(ff8800), similarity 0.2, opacity 0.5",
}}}

hl.window_rule({ match = { class = "^(kitty)$" }, ["plugin:chromakey"] = "1" })

hl.on("hyprland.start", function() hl.exec_cmd("LAUNCHER") end)
