# Griddy

One-click source layouts for OBS Studio. Podcasts, eSports, and custom grids are applied in a couple of clicks.

## Features

- **Griddy dock** (Docks → Griddy):
  - **Canvas:** switch between Main and extra canvases such as [Vertical Canvas](https://aitum.tv/products/vertical) (OBS 31.1+), each shown with its resolution. Only layouts matching the canvas orientation are listed.
  - **Selection banner:** shows how many sources are selected in OBS, so you know what a click will do.
  - **Gallery:** layouts are grouped (Podcast, eSports, Overlays, Portrait) and searchable.
  - **One click:** select sources in OBS (top of the Sources list = slot 1), then click a layout.
  - **Pick & apply:** with nothing selected, choose a source for each numbered slot and press **Apply to "Scene"**.
  - **Live preview:** the selected layout is drawn with each slot's source name.
  - **Rotate** moves each source to the next slot. This is handy for swapping who's main.
  - **Adjust this layout** (collapsible): **Gap**, **Margin** and **Main size** sliders, **Aspect lock**, **Mirror** (flip left/right) and **Safe zone**.
  - **Safe zone:** shade the area covered by TikTok, YouTube Shorts or Instagram Reels buttons and captions (approximate), and optionally **Keep inside** it.
- **Built-in presets:**
  - *Podcast / panels:* Side by Side, Three Across, 2x2 Grid, 3x3 Grid, Interview, Talk Show
  - *eSports / gaming:* Main + Stacked, Main + Side Column (3 and 4 cams), Main + Bottom Strip, Main + Flanking, L-Shape (1 + 5, use **Mirror** for cams on the right), Versus
  - *Overlays:* Picture in Picture, Two Corner PiP, Screen + Facecam
  - *Portrait canvas:* Vertical: Game + Cam, Game + 2 Cams, Cropped Game + Cam, Three Stacked, Top / Bottom Split, Cam + Game PiP
- **Layout editor** (Tools → Griddy Layout Editor, or **Edit Layout…** in the dock):
  - Toolbar for the name, Grid/Free mode and orientation. **Layout** and **Slot** tabs on the right; clicking a slot opens its tab.
  - **Group:** the gallery section a layout is listed under. Type a new name to make a new section.
  - **Grid mode:** set column and row weights (e.g. `2, 1`). Click a cell to add a slot, or drag across cells to merge them into one slot.
  - **Free mode:** draw slots anywhere, drag to move them, and drag a corner to resize. Snaps to a 1/48 grid.
  - **Orientation:** Any, Landscape or Portrait. This decides which canvases the layout is listed for.
  - **Per-slot options:** fit mode (crop to fill, fit inside, stretch), locked aspect ratio, and slot order.
  - **More** menu: import/export layouts as JSON, delete a layout, or reset to the built-in presets.
- Applying a layout is **one-shot and undoable** (Ctrl+Z). It sets the item's position and bounding box, so the items stay normal OBS sources you can tweak afterwards. Existing crops are kept.
- In Studio Mode, layouts apply to the preview scene. On other canvases they apply to that canvas's active scene; press **Refresh** after switching scenes there.
- Already have a saved `layouts.json`? Use **Reset Defaults** in the editor to get the newest presets (portrait layouts, the left-hand L-Shape).

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
