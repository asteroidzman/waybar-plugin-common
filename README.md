# waybar-plugin-common

Shared header-only helpers for the asteroidzman waybar CFFI plugins
(sysmon, network, volume, medication, weather, media-cava).

- **WbPop** — content-sized layer-shell popup under a bar pill: blurred
  (`waybar-popup` namespace), clamped on-screen on every edge, Escape +
  debounced focus-loss dismissal, cross-plugin singleton, keyboard-nav hook.
- **wb_icon_*** — monochrome SVG icons tinted to the GTK theme colour,
  re-tinted automatically on theme change.
- **WbReader** — line-reader subprocess that dies with the bar
  (`PR_SET_PDEATHSIG`) and respawns with backoff if the child exits.

Consumed as a git submodule at `common/` in each plugin repo:

```sh
git clone --recursive https://github.com/asteroidzman/waybar-<plugin>.git
```
