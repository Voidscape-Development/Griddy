# Griddy

One-click source layouts for OBS Studio. Podcasts, eSports, and custom grids are applied in a couple of clicks.

## Features

- **Griddy dock** (Docks → Griddy): a visual list of layouts.
  - **One click:** select sources in OBS (top of the Sources list = slot 1), then click a layout.
  - **Pick & apply:** with nothing selected, choose a source for each slot and press **Apply**.
  - **Rotate** moves each source to the next slot. This is handy for swapping who's main.
  - Each layout has its own settings: **Gap**, **Margin**, **Main size %**, **Aspect lock** and **Mirror** (flip left/right).
- **Built-in presets:**
  - *Podcast / panels:* Side by Side, Three Across, 2x2 Grid, 3x3 Grid, Interview, Talk Show
  - *eSports / gaming:* Main + Stacked, Main + Side Column (3 and 4 cams), Main + Bottom Strip, Main + Flanking, L-Shape (1 + 5), Versus
  - *Overlays:* Picture in Picture, Two Corner PiP, Screen + Facecam
  - *Portrait canvas:* Vertical: Game + Cam
- **Layout editor** (Tools → Griddy Layout Editor, or **Edit Layouts…** in the dock):
  - **Grid mode:** set column and row weights (e.g. `2, 1`). Click a cell to add a slot, or drag across cells to merge them into one slot.
  - **Free mode:** draw slots anywhere, drag to move them, and drag a corner to resize. Snaps to a 1/48 grid.
  - **Per-slot options:** fit mode (crop to fill, fit inside, stretch), locked aspect ratio, and slot order.
  - Import/export layouts as JSON, or reset them to the built-in presets.
- Applying a layout is **one-shot and undoable** (Ctrl+Z). It sets the item's position and bounding box, so the items stay normal OBS sources you can tweak afterwards. Existing crops are kept.
- In Studio Mode, layouts apply to the preview scene.

### How aspect lock works

Slot 1 is the main slot. In grid mode, column weights set the widths, and a locked slot sets the height of its rows. The whole group is then shrunk if needed and centered on the canvas. For example, **Main + Stacked** gives a 16:9 game with two cams stacked beside it at the same total height. In free mode, a locked slot is fitted inside its rectangle.

Layouts are stored globally in `plugin_config/griddy/layouts.json`, in the OBS config folder.

## Building

Griddy is based on the [OBS plugin template](https://github.com/obsproject/obs-plugintemplate) and needs Qt 6 and the obs-frontend-api. See the template wiki for [build system requirements](https://github.com/obsproject/obs-plugintemplate/wiki/Build-System-Requirements).

```sh
cmake --preset ubuntu-x86_64   # or macos / windows-x64
cmake --build --preset ubuntu-x86_64
```

CI (GitHub Actions) builds and packages Windows, macOS and Ubuntu.
