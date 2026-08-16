# hyprchromakey

A Hyprland plugin that makes chosen background colors transparent, per pixel, while keeping elements like text, icons, and images fully opaque.

Unlike window opacity, which fades a whole window uniformly, hyprchromakey only touches pixels that
match a color you nominate. A terminal with a solid background becomes a floating layer of text.

Supports **Hyprland 0.56 and up**. Plugins have no stable ABI, so it tracks current Hyprland
rather than older releases.

---

## Quick start

```lua
-- the color to key out, and where to apply it
hl.config { plugin = { hyprchromakey = { keys = "rgb(1e1e2e)" } } }

hl.window_rule({ match = { class = "^(kitty)$" }, ["plugin:chromakey"] = "1" })
```

That's the whole minimum setup. `hyprctl chromakey` shows what the plugin thinks is going on,
which is the first thing to check if nothing happens.

Using a `.conf` config instead? Every example here has a hyprlang equivalent. See
[hyprlang config](#hyprlang-config).


## Choosing what gets keyed

Targets are selected with ordinary window and layer rules, using the `plugin:chromakey` effect the
plugin registers. The value is `1`/`on` to key with the default profile, `0`/`off` to exclude, or
the name of a [profile](#profiles).

The effect is a dynamic one, so in Lua it goes in as a quoted key:

```lua
hl.window_rule({ match = { class = "^(kitty|Alacritty)$" }, ["plugin:chromakey"] = "1" })
hl.window_rule({ match = { title = "^(.*YouTube.*)$" },     ["plugin:chromakey"] = "0" })   -- exclude
hl.window_rule({ match = { class = "^(foot)$" },            ["plugin:chromakey"] = "term" }) -- named profile

hl.layer_rule({ match = { namespace = "^(waybar)$" },       ["plugin:chromakey"] = "1" })
```

Any `match:` property Hyprland supports works, so you can scope by `class`, `title`, `floating`,
`fullscreen`, `xwayland`, `workspace`, `tag` and the rest.

### Keying everything, with exceptions

If most of your windows should be keyed, flip the default and list the exceptions instead:

```lua
hl.config { plugin = { hyprchromakey = {
    default_windows = "on",   -- off | on | <profile name>
    default_layers  = "off",
}}}

hl.window_rule({ match = { class = "^(firefox|mpv|imv)$" }, ["plugin:chromakey"] = "0" })
```

Rules always win over the default, so a single `plugin:chromakey 0` line carves a window out.


## Key colors

Keys go in the `keys` string, `;`-separated. Every key in a profile is tested against every pixel,
so list as many as you like. `table.concat` keeps it readable:

```lua
hl.config { plugin = { hyprchromakey = { keys = table.concat({
    "rgb(1e1e2e)",
    "color rgb(313244), similarity 0.04, opacity 0.4",
    "color #11111b, match hsv, smoothness 0.05",
}, "; ") } } }
```

Within one key, fields are comma-separated `name value` pairs in any order. A lone value is taken
as the color.

| field | default | meaning |
| --- | --- | --- |
| `color` | required | key color; `rgb()`, `rgba()`, `#rrggbb`, `#rgb`, `0xAARRGGBB` |
| `similarity` | `similarity` global | distance below which a pixel is fully keyed |
| `smoothness` | `smoothness` global | fade band just above `similarity`, so antialiased glyph edges don't get a hard outline |
| `opacity` | `opacity` global | alpha a fully keyed pixel ends up with; `0` is invisible, `0.4` is a tint |
| `match` | `match` global | how the distance is measured, see below |
| `profile` | `default` | which profile this key belongs to |

### Match modes

| mode | behaviour | good for |
| --- | --- | --- |
| `rgb` | largest per-channel difference | flat UI colors; the one you want almost always |
| `hsv` | weighted hue, saturation and value difference | colors that shift slightly between themes or states |
| `chroma` | hue and saturation only, brightness ignored entirely | shaded or gradient backgrounds of one color |

`chroma` deliberately ignores how light or dark a pixel is, so it keys a whole gradient, but for
the same reason, it treats black, white and grey as the same color. Use it with saturated key
colors, not with the near-black backgrounds most terminals use.

### Profiles

Give keys a `profile` to group them, then point rules at that name. Keys without one land in
`default`.

```lua
hl.config { plugin = { hyprchromakey = { keys = table.concat({
    "rgb(1e1e2e)",                                  -- -> default
    "profile term, color rgb(0b0b12)",
    "profile term, color rgb(1a1a24), opacity 0.3",
}, "; ") } } }

hl.window_rule({ match = { class = "^(kitty)$" }, ["plugin:chromakey"] = "term" })
```

A profile holds up to 16 keys.


## Options

All under `plugin:hyprchromakey:`.

```lua
hl.config { plugin = { hyprchromakey = {
    enabled = true,

    -- defaults for keys that don't say otherwise
    similarity = 0.08,
    smoothness = 0.02,
    opacity    = 0.0,
    match      = "rgb",

    -- only key pixels the client drew (nearly) opaque, so already-translucent
    -- areas are left alone
    min_alpha = 0.99,

    -- applied to anything without a matching rule
    default_windows = "off",
    default_layers  = "off",

    -- key only a window's main surface, leaving its popups and menus opaque
    main_surface_only = false,

    -- see "why it makes keyed windows a hair translucent" below
    force_translucent = true,
    translucency      = 0.9995,

    -- the key colors themselves, `;`-separated
    keys = "",
}}}
```

### Tuning

- Nothing keyed? Your color is probably slightly off. Screenshot the window and pick the
  background pixel with a color picker, such as [hyprpicker](https://github.com/hyprwm/hyprpicker). The value you want is the exact sRGB one.
- Text has a dark halo? That's antialiasing blending into the background color. Raise
  `smoothness` a little; raising `similarity` too far starts eating the glyph edges instead.
- Parts of an image or icon disappear? Lower `similarity`, or switch that key to `hsv`.


## hyprlang config

Everything above works the same in a `.conf` config, with one addition: key colors can be written
as repeatable `chromakey` keywords instead of the `keys` string. Both accept identical field
syntax, and you can use either.

```conf
# hyprland.conf
chromakey = rgb(1e1e2e)
chromakey = color rgb(181825), similarity 0.04
chromakey = profile term, color rgb(11111b), similarity 0.05

plugin {
    hyprchromakey {
        enabled    = true
        similarity = 0.06
        smoothness = 0.02

        default_windows = off
        default_layers  = off
    }
}

windowrule = plugin:chromakey 1,    match:class ^(kitty)$
windowrule = plugin:chromakey term, match:class ^(Alacritty)$
windowrule = plugin:chromakey 0,    match:class ^(mpv|imv)$
layerrule  = plugin:chromakey 1,    match:namespace ^(waybar)$

bind = SUPER, T, exec, hyprctl dispatch chromakey:toggle
```

## Dispatchers

`hl.dsp` only covers Hyprland's built-in dispatchers, so plugin ones are reached through
`exec_cmd`:

```lua
hl.bind("SUPER + T", hl.dsp.exec_cmd("hyprctl dispatch chromakey:toggle"))
```

| dispatcher | what it does |
| --- | --- |
| `chromakey:toggle [profile]` | toggles keying on the active window |
| `chromakey:togglewindow <window> [profile]` | same, for a window you name |
| `chromakey:set <on\|off\|profile>` | sets the active window explicitly |
| `chromakey:setwindow <window> <on\|off\|profile>` | sets a window you name |
| `chromakey:reset` | drops all manual overrides, back to the rules |
| `chromakey:reload` | rebuilds profiles and shaders |

`<window>` is `class:<regex>`, `title:<regex>`, `address:0x...`, or a bare regex tried against
both. Manual overrides outrank rules until `chromakey:reset`.


## Inspecting

```bash
hyprctl chromakey
```

Prints whether the hooks installed, every profile and key as the plugin parsed it, and which
windows and layers are keyed right now. `hyprctl -j chromakey` gives the same as JSON.


## Installation

### hyprpm

```bash
hyprpm add https://github.com/TheWoomy27/hyprchromakey
hyprpm enable hyprchromakey
hyprpm reload
```

### Manually

```bash
make
hyprctl plugin load "$PWD/out/hyprchromakey.so"
```

Plugins have no stable ABI, so rebuild after every Hyprland update. The plugin logs a warning if
it detects it was built against a different Hyprland commit than the one running.

## How it works

Hyprland compiles a fragment shader variant per (shader, feature-flag) combination and picks one
for every texture it draws. hyprchromakey hooks that choice: when the surface being drawn belongs to
a keyed window or layer, it hands back its own copy of the very same shader with the key test
spliced in right after the texture sample.

Because it is still Hyprland's shader, everything downstream keeps working: color management,
rounded corners, tint, blur blending, motion blur. New feature combinations are patched the first
time they are requested, and if patching or compiling ever fails the stock shader is used instead,
so a future Hyprland change degrades to "no effect" rather than a broken screen.

### Why it makes keyed windows a hair translucent

Hyprland skips compositing anything behind a surface it believes is fully opaque, and disables
blending while drawing it. A window that is opaque as far as Hyprland is concerned would key out
to nothing at all. So three things happen to keyed surfaces, all under `force_translucent`:

- their alpha is nudged to `translucency` (0.9995 by default), well under one 8-bit step;
  invisible, but enough that Hyprland composites and blends behind them;
- `CWindow::opaque()` reports false for them, because a keyed window genuinely isn't opaque;
- the **opaque region the client declares** is cleared each frame.

The second one matters more than it looks. `opaque()` reads the window's *own* alpha, which is
below 1 while an open or move animation is running and exactly 1 once it settles, so without it
blur switches on for the duration of every animation and off again at rest, and a fullscreen keyed
window becomes eligible for direct scanout, which would bypass compositing altogether.

The third is the subtle one. A client declares which parts of its surface are fully opaque, and a
keyed window's client has no idea we are about to punch holes in it, so it goes on declaring itself
opaque. Once such a surface is drawn at alpha >= 1, Hyprland inverts that region, finds nothing
translucent left to composite behind, and switches blur off for it - and the same region drives
occlusion culling. That only bites when something returns alpha to exactly 1 mid-animation, which
is what an **overshoot** bezier does on its way past the target, so blur would snap off for
precisely the length of the overshoot. Clearing the declared region fixes both, and is simply true
of a keyed surface.

Set `force_translucent = false` to turn all three off; keying will then only work on windows that
are already translucent for other reasons.

### Blur

A keyed window counts as translucent, so Hyprland blurs behind it whenever `decoration:blur` is
on, including behind the pixels you keyed out. That is usually the point. To keep the keying but
drop the blur, use the stock rule alongside your chromakey one:

```lua
hl.window_rule({ match = { class = "^(kitty)$" }, ["plugin:chromakey"] = "1", no_blur = true })
```

Blur behind a keyed window is not free: Hyprland is now compositing and blurring a region it used
to skip entirely. On a large keyed window that costs real GPU time.

## Known issues

**Flicker during animations that use an overshoot bezier.** With a curve whose control point goes
past 1.0 (`overshot`) on an animation that fades (`fade` or `slidefade`), keyed windows visibly flicker for the portion of the curve that is past its target. It
looks like blur switching off and back on.

What is known: it does not happen with a curve that stays within 0..1, it does not happen with the
plugin unloaded, and it is independent of a key's `opacity`. Instrumenting Hyprland's own decisions
shows keying and blur both staying active throughout, so the cause is somewhere other than the two
obvious candidates and is currently unidentified.

Workaround: use a non-overshooting curve for animations that fade.


## Tests

```bash
make -C tests HYPRLAND_SRC=/path/to/Hyprland/checkout
```

- `chroma_math_test` renders known colors through the key shader on a real GPU and checks the
  resulting alpha: thresholds, the smooth band, multiple keys, the alpha gate, each match mode.
- `shader_patch_test` preprocesses Hyprland's own shader sources exactly as Hyprland does, splices
  the chroma code into every feature variant, and compiles each one. **This is the test that tells
  you a new Hyprland release moved something.**

There is also an end-to-end check that runs a throwaway nested Hyprland, loads the plugin into it
and screenshots the result. Your own session is never involved:

```bash
./tests/run-nested.sh with-plugin /tmp/hcrun
```


## Prior art

The idea of chroma keying a window's background in Hyprland comes from
[Hypr-DarkWindow](https://github.com/micha4w/Hypr-DarkWindow) and the shader experiments around it.
This plugin shares no code with it. It is a fresh implementation built on Hyprland 0.56's shader
variant system, adding multiple key colors, profiles, match modes and layer support.
