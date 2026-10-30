# hyprchromakey

A Hyprland plugin that makes chosen background colors transparent, per pixel, while keeping elements like text, icons, and images fully opaque.

Unlike window opacity, which fades a whole window uniformly, hyprchromakey only touches pixels that
match a color you nominate. 

Supports **Hyprland 0.57 and up**. Plugins have no stable ABI, so it tracks current Hyprland
rather than older releases. On 0.56, hyprpm still installs the last build that supports it; to
build that one by hand, check out `d730637`.

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


## Configuration

Everything, with every option at its default. Copy it, change the colors, delete what you don't
need.

```lua
hl.config { plugin = { hyprchromakey = {
    enabled = true,

    -- defaults for keys that don't override them
    similarity = 0.08,
    smoothness = 0.02,
    opacity    = 0.0,
    match      = "rgb",

    -- only key pixels the client drew (nearly) opaque
    min_alpha = 0.99,

    -- applied to anything without a matching rule: off | on | <profile name>
    default_windows = "off",
    default_layers  = "off",

    -- key only a window's main surface, leaving popups and menus alone
    main_surface_only = false,

    -- leave fullscreen windows unkeyed (maximized ones still are)
    ignore_fullscreen = false,

    -- only key flat patches of a key color at least this many pixels across, so images and
    -- video that happen to brush the color stay intact. 0 is off; 8 is a good start
    min_region = 0,

    -- makes keyed surfaces count as translucent so hyprland composites and blurs behind them. 
    force_translucent = true,
    translucency      = 0.9995,

    -- the key colors, ";"-separated. Fields within a key are "name value", comma separated
    keys = table.concat({
        "rgb(1e1e2e)",
        "color rgb(181825), similarity 0.04",
        "color rgb(313244), opacity 0.4, match hsv, region 12",
        "profile term, color rgb(11111b), similarity 0.05, smoothness 0.03",
    }, "; "),
}}}

-- what gets keyed
hl.window_rule({ match = { class = "^(kitty|Alacritty)$" }, ["plugin:chromakey"] = "1" })
hl.window_rule({ match = { class = "^(foot)$" },            ["plugin:chromakey"] = "term" })
hl.window_rule({ match = { class = "^(mpv|imv)$" },         ["plugin:chromakey"] = "0" })
hl.layer_rule({  match = { namespace = "^(waybar)$" },      ["plugin:chromakey"] = "1" })
```

`hyprctl chromakey` shows what the plugin made of all that, and is the first thing to check if
nothing happens.

Hyprland 0.57 reads Lua configs only, so there is no `hyprland.conf` form any more.


## Options

All under `plugin:hyprchromakey:`.

| option | type | default | meaning |
| --- | --- | --- | --- |
| `enabled` | bool | `true` | master switch |
| `similarity` | float | `0.08` | default distance below which a pixel is fully keyed |
| `smoothness` | float | `0.02` | default fade band above `similarity`, so antialiased glyph edges don't get a hard outline |
| `opacity` | float | `0.0` | default alpha a fully keyed pixel ends up with; `0` is invisible, `0.4` is a tint |
| `match` | string | `rgb` | default comparison: `rgb`, `hsv` or `chroma` |
| `min_alpha` | float | `0.99` | only key pixels whose source alpha is at least this, so already-translucent areas are left alone |
| `default_windows` | string | `off` | applied to windows with no matching rule: `off`, `on` or a profile name |
| `default_layers` | string | `off` | same, for layer surfaces |
| `main_surface_only` | bool | `false` | key only the main surface, leaving popups and subsurfaces opaque |
| `ignore_fullscreen` | bool | `false` | leave fullscreen windows unkeyed, so they can be scanned out directly again. Maximized windows are still keyed |
| `min_region` | int | `0` | default for the `region` field below: only key flat patches of the color at least this many pixels across. `0` is off |
| `force_translucent` | bool | `true` | make keyed surfaces count as translucent; without it keying has nothing to reveal |
| `translucency` | float | `0.9995` | the alpha `force_translucent` uses. `1.0` disables the nudge |
| `keys` | string | `""` | the key colors, `;`-separated |


## Key colors

A key is a comma-separated list of `name value` fields, in any order. A lone value is taken as the
color, so `rgb(1e1e2e)` on its own is a valid key.

| field | default | meaning |
| --- | --- | --- |
| `color` | required | `rgb()`, `rgba()`, `#rrggbb`, `#rgb` or `0xAARRGGBB` |
| `similarity` | the global | distance below which a pixel is fully keyed |
| `smoothness` | the global | fade band just above `similarity` |
| `opacity` | the global | alpha a fully keyed pixel ends up with |
| `match` | the global | how the distance is measured |
| `region` | `min_region` | smallest flat patch of the color that gets keyed, in pixels; see below |
| `profile` | `default` | which profile this key belongs to |

Every key in a profile is tested against every pixel, up to 16 per profile. A `similarity` of `0`
means the exact same 8-bit color.

### Match modes

| mode | behaviour | good for |
| --- | --- | --- |
| `rgb` | largest per-channel difference | flat UI colors; the one you want almost always |
| `hsv` | weighted hue, saturation and value difference | colors that shift slightly between themes or states |
| `chroma` | hue and saturation only, brightness ignored entirely | shaded or gradient backgrounds of one color |

`chroma` deliberately ignores how light or dark a pixel is, so it keys a whole gradient, but for
the same reason it treats black, white and grey as the same color. Use it with saturated key
colors, not the near-black backgrounds most terminals use.

### Profiles

Keys with a `profile` field are grouped under that name; keys without one land in `default`. Point
a rule at a profile by using its name as the rule's value.

### Keeping images and video intact

A key matches colors, and a photo or a dark video scene inside a keyed window can come close
enough to a key color to get holes punched in it - mostly through the `smoothness` band, which is
why turning `similarity` down doesn't help much.

`region` (or `min_region` for every key) fixes that by looking at the pixel's neighbors, not just
the pixel. A pixel is only keyed if it sits on a horizontal or vertical stretch of the exact key
color at least that many pixels long, or in a smaller patch walled in by clear edges, like the
inside of an `o`. Backgrounds are big flat areas and pass easily, right up against text. Media
almost never does: its pixels fade into near misses of the key instead of repeating it exactly.

Start at `8`. Measured on a synthetic dark video frame against a matching key, `0` let 99.9% of
the frame show through, `4` about 0.6%, and `8` none of it, while 11 pt terminal text keyed
virtually the same as with it off. The length is in the window's own buffer pixels, so on a scaled monitor allow for the scale.

What it cannot tell apart is media that genuinely contains a flat patch of the key color, such as
a screenshot of the same app, or a lone key-colored speck in an otherwise bright image. It costs a
handful of extra texture samples per keyed pixel, and nothing when off.


## Targeting

Rules select what gets keyed, using the `plugin:chromakey` effect the plugin registers. The value
is `1`/`on` for the default profile, `0`/`off` to exclude, or a profile name.

Any `match:` property Hyprland supports works, so you can scope by `class`, `title`, `floating`,
`fullscreen`, `xwayland`, `workspace`, `tag` and the rest.

To key most things and list the exceptions instead, set `default_windows = "on"` and use `0` rules
to carve windows out. Rules always beat the default.

`ignore_fullscreen` is the one thing that beats a rule: a fullscreen window is left alone whatever
its rules say. That gets fullscreen games and video back to direct scanout, which matters on a
laptop. A manual override from the functions below still wins over it.


## Controlling it at runtime

The plugin exposes functions under `hl.plugin.hyprchromakey`. Bind them through a closure:

```lua
hl.bind("SUPER + K", function() hl.plugin.hyprchromakey.toggle_all() end)
```

Binding the bare function works too, but it keeps a reference to the plugin's code, which goes
stale if the plugin is unloaded or updated without a config reload. The closure looks the function
up each time.

| function | what it does |
| --- | --- |
| `toggle_all()` | turns the whole effect on or off |
| `set_all(value)` | `"on"`, `"off"`, or `"config"` to follow `enabled` again |
| `toggle([profile])` | toggles keying on the active window |
| `toggle_window(window, [profile])` | same, for a window you name |
| `set(value)` | sets the active window: `"on"`, `"off"` or a profile name |
| `set_window(window, value)` | same, for a window you name |
| `reset()` | drops every override, back to the config and the rules |
| `reload()` | rebuilds profiles and shaders |

`window` is `class:<regex>`, `title:<regex>`, `address:0x...` (as `hyprctl chromakey` prints it),
or a bare regex tried against both. Overrides outrank the config and the rules until reset. Naming a
window or profile that doesn't exist raises an error; the active-window functions just do nothing
when no window is focused.

From a terminal, `hyprctl eval` runs the call directly:

```bash
hyprctl eval 'hl.plugin.hyprchromakey.toggle_all()'
```

`hyprctl dispatch` also evaluates Lua, but hands what it gets to `hl.dispatch`, so pass the
function rather than calling it — bare for no arguments, wrapped otherwise:

```bash
hyprctl dispatch 'hl.plugin.hyprchromakey.toggle_all'
```

```bash
hyprctl dispatch 'function() hl.plugin.hyprchromakey.set_all("off") end'
```

Hyprland 0.57 removed plugin dispatchers, so these functions are the plugin's whole runtime
interface; the old `chromakey:*` dispatchers are gone.

### Why not just write to `enabled`?

You can change any config value at runtime with
`hyprctl eval 'hl.config { plugin = { hyprchromakey = { enabled = false } } }'`, and the plugin
will pick it up, including key colors. But Hyprland does not announce plugin config changes,
so the plugin can only notice one while it is drawing a frame, and a compositor with nothing moving
on screen draws no frames at all. On an idle desktop the change then sits unapplied until something
else causes a redraw, which looks like windows updating only once you touch them.

The switches above are the plugin's own, not a config value, so they repaint every keyed window the
moment you run them. Use those for anything interactive, and `eval` for values you are tuning.


## Inspecting

```bash
hyprctl chromakey
```

Prints whether the hooks installed, every profile and key as the plugin parsed it, and which
windows and layers are keyed right now. `hyprctl -j chromakey` gives the same as JSON.


## Tuning

- Nothing keyed? Your color is probably slightly off. Screenshot the window and pick the
  background pixel with a color picker; the value you want is the exact sRGB one.
- Text has a dark halo? That's antialiasing blending into the background color. Raise
  `smoothness` a little; raising `similarity` too far starts eating the glyph edges instead.
- Parts of an image or video disappear? Set `min_region` (see
  [Keeping images and video intact](#keeping-images-and-video-intact)). For icons, lower
  `similarity` or switch that key to `hsv`.



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
- the window's `opaque()` check reports false for them, because a keyed window genuinely isn't opaque;
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

Only the window's own pixels are keyed. With blur on, Hyprland draws the blurred backdrop behind
a window as a separate texture, and that one is left alone, so blur never gets holes in it where
the blurred wallpaper happens to come near a key color.

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
  resulting alpha: thresholds, the smooth band, multiple keys, the alpha gate, each match mode,
  and `min_region` against a synthetic window with text, a letter counter and noisy media.
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
This plugin shares no code with it. It is a fresh implementation built on Hyprland's shader
variant system, adding multiple key colors, profiles, match modes and layer support.
