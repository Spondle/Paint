# Paint

A fast, GNOME-native Paint application written in **C** with **GTK 4**, **Libadwaita**, and **Cairo**, styled after **GNOME Text Editor** and **Ptyxis**.

## Features

- **Multi-Layer & OpenRaster (`.ora`) Editing**: Add, duplicate, drag-reorder, rename, solo (`Alt+Click`), opacity/blend-mode adjust (`Normal`, `Multiply`, `Screen`, `Overlay`, `Darken`, `Lighten`), Alpha Lock, Layer Lock, Copy Merged (`Ctrl+Shift+C`), Paste as New Layer (`Ctrl+Alt+V`), Import Image as Layer, and save/open layered `.ora` projects cleanly alongside PNG, JPEG, WebP (when `libwebp` is installed), and BMP.
- **Precision Drawing & Selection Tools**: Rectangle, Ellipse, Freehand Lasso, and Magic Wand selections with 8 resize handles (`Shift` aspect lock) and Invert Selection (`Ctrl+Shift+I`); Brush presets (`Smooth`, `Soft`, `Pixel`), Line, 9 Shapes, Eraser, Tolerance-aware Flood Fill with Sample All Layers, Linear & Radial Gradient (`G`), Blur & Pixelate retouch brushes, Transparency Brush/Fill, and Stylus tablet pressure & eraser sensitivity.
- **Rich Multiline Text & Colors**: Draggable multiline Text overlay with `GtkFontDialogButton`, alignment, outline stroke, and background pill; overlapping Primary/Secondary swatches (`Shift+X` swap, `D` B/W reset), persisted Recent Colors, and customizable 16-slot Palette.
- **Image Adjustments & Filters**: Live canvas-preview `Brightness & Contrast…`, `Hue & Saturation…`, `Gaussian Blur…`, plus `Sharpen`, `Invert Colors`, `Grayscale`, and `Trim Transparent`, respecting active selections and Alpha Lock.
- **Fast Canvas UX**: Dual-tier zoom cache (5%–3200%), cursor coords & selection dimensions HUD, draggable East/South/SE canvas edge resize handles, Spacebar-drag panning, right-click context menu, Print (`Ctrl+P`), memory-budgeted tiled undo/redo (`256 MB`), customizable shortcuts, atomic session restore & autosave, and `gettext` internationalization scaffolding.

## Building & Running

Optional dependencies: `libwebp` (`libwebp-dev` / `libwebp-devel`) enables WebP export alongside PNG, JPEG, BMP, and OpenRaster (`.ora`).

```bash
meson setup build
meson compile -C build
./build/paint
```

To install locally for your user (adds the app & icon to GNOME's app launcher):

```bash
meson setup --reconfigure build --prefix="$HOME/.local"
meson install -C build
```

## Packaging & Updates

### Flatpak (Cross-distro + GNOME Software)

1. Build and install locally with `flatpak-builder`:
   ```bash
   sudo dnf install flatpak-builder
   flatpak install flathub org.gnome.Sdk//51
   flatpak-builder --user --install --force-clean build-flatpak packaging/flatpak/io.github.spondle.Paint.yml
   ```
2. Automated GitHub Releases: pushing a version tag (`git tag v0.1.0 && git push origin v0.1.0`) triggers [ci-release.yml](.github/workflows/ci-release.yml) to build and attach `io.github.spondle.Paint.flatpak` to a GitHub Release. Users can double-click it in GNOME Software or run:
   ```bash
   flatpak install --user io.github.spondle.Paint.flatpak
   ```

### Fedora COPR (Native `dnf` install & updates)

1. Create a project on [copr.fedorainfracloud.org](https://copr.fedorainfracloud.org/) and add a package backed by `https://github.com/Spondle/Draw` using `packaging/rpm/paint.spec`.
2. Users install and receive automatic updates with:
   ```bash
   sudo dnf copr enable spondle/draw
   sudo dnf install paint
   ```

### Debian / Ubuntu (`.deb` package)

On Debian 13+ or Ubuntu 24.10+ (requires Libadwaita ≥ 1.6):

```bash
sudo apt install debhelper dpkg-dev meson pkgconf appstream desktop-file-utils \
  libgtk-4-dev libadwaita-1-dev libcairo2-dev
dpkg-buildpackage -us -uc -b
sudo apt install ../paint_*.deb
```

## Architecture

- [`meson.build`](meson.build) — Meson build configuration, `GResource` bundling, `gettext` (`po/`), unit tests, and install targets
- [`src/main.c`](src/main.c) & [`src/paint-application.c`](src/paint-application.c) — `AdwApplication`, resources, preferences/tool settings sync, shortcuts, About dialog
- [`src/paint-window.c`](src/paint-window.c) — `AdwApplicationWindow`, `AdwHeaderBar`, `AdwTabView` tear-off, `AdwTabOverview`, printing, menus, actions, and drag-and-drop
- [`src/paint-canvas-page.c`](src/paint-canvas-page.c) — Scrollable/zoomable workspace, canvas edge resize handles, bottom-left coords HUD, Spacebar pan, context menu, and floating Adwaita OSD bars
- [`src/paint-canvas.c`](src/paint-canvas.c) — `PaintCanvas` widget: 2-tier viewport/overview cache, stylus pressure input, selection handles/masks, text overlay, and live filter preview sessions
- [`src/paint-document.c`](src/paint-document.c) — `PaintDocument` multi-layer model, masked compositing, layer locks, memory-budgeted tiled undo/redo, and canvas transforms
- [`src/paint-io.c`](src/paint-io.c) — GIO stream image codecs (`PNG`, `JPEG`, `WebP`, `BMP`) and pure-C `.ora` (OpenRaster) reader/writer
- [`src/paint-filters.c`](src/paint-filters.c) — Premultiplied-alpha image adjustments (`Brightness & Contrast`, `Hue & Saturation`, `Gaussian Blur`, `Sharpen`, `Invert`, `Grayscale`)
- [`src/paint-tools.c`](src/paint-tools.c) — Extensible `PaintTool` registry (Selection modes, Brush, Line, Eraser, tolerance Fill, Gradient, Blur/Pixelate, Text, Eyedropper, Transparency, Shapes)
- [`src/paint-color-picker.c`](src/paint-color-picker.c) — Dual Primary/Secondary color swatches, recent colors, custom palette, and popover editor
- [`src/paint-layers-panel.c`](src/paint-layers-panel.c) — Collapsible Layers sidebar with drag-and-drop reordering, solo toggling, alpha/layer locks, opacity, and blend modes
- [`src/paint-session.c`](src/paint-session.c) — Atomic session persistence (`~/.local/share/paint`) and debounced 30s autosave
- [`src/paint-dialogs.c`](src/paint-dialogs.c) — Resize & Scale, Export options, and live-preview filter dialogs
- [`src/paint-preferences.c`](src/paint-preferences.c) — `AdwPreferencesDialog` (Appearance, Behavior, Stylus, Palette/Shortcuts) and theme selector
- [`src/paint-i18n.h`](src/paint-i18n.h) & [`po/`](po/POTFILES) — `gettext` localization headers and extraction list

## Testing

```bash
meson test -C build -v
```
