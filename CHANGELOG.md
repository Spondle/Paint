# Changelog

All notable changes to Paint will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.3.0] - 2026-11-21

### Added
- Multi-layer editing with add, duplicate, reorder, rename, visibility toggle, opacity slider, and 6 blend modes (`Normal`, `Multiply`, `Screen`, `Overlay`, `Darken`, `Lighten`)
- Collapsible Layers sidebar (`Ctrl+L`) with live thumbnails, Merge Down, Flatten Layers, per-layer tiled undo/redo, and multi-layer session persistence
- Rectangle Selection tool (`M`) with drag move, arrow-key nudging (`Shift` for 10 px steps), Cut/Copy/Paste, Paste as New Canvas (`Ctrl+Shift+V`), and Crop to Selection (`Ctrl+Shift+X`)
- Text tool (`X`) with live draggable preview, font family (`Sans`, `Serif`, `Monospace`), size, and Bold/Italic styling
- Eyedropper / Color Picker tool (`I`) with one-shot auto-return, live hold-`Alt` picking override, and cursor-adjacent picked color swatch preview
- Brush style presets (`Smooth`, `Soft`, `Pixel`) inside the Brush Size & Style popover
- Recent Colors history (up to 8 swatches) in the color popover
- Block Arrow shape (outline and filled) plus Spacebar-drag repositioning while drawing lines and shapes
- Canvas Resize & Scale dialog (canvas resize or smooth/nearest-neighbor scale with aspect ratio lock) and Rotate 90° CW/CCW / Flip Horizontal/Vertical transforms
- Fit to Window (`Ctrl+9`), high-zoom Pixel Grid overlay at 800%+ zoom, and drag-and-drop image opening
- Customizable keyboard shortcuts in **Preferences → Shortcuts** with key capture, conflict detection, shortcut disabling, and Reset / Reset All actions
- Startup **Behavior** preferences: **Startup Window Size** (`Remember Previous Size`, `Fit Window to Canvas`, `Default Size`) and **Fit Canvas to Window on Startup**

### Changed
- Made the bottom floating toolbar more compact and organized Brush Size & Style into a vertical popover
- Made the primary menu more compact with symmetrical zoom controls
- **Keyboard Shortcuts** (`Ctrl+?` / primary menu) now opens directly to the **Shortcuts** tab inside Preferences
- Improved single-tab and multi-tab unsaved-changes confirmation dialogs

### Packaging & CI
- Added automated GitHub Actions builds for `.deb` (Debian Trixie), `.rpm` (Fedora), and `.flatpak` packages alongside changelog-backed GitHub Release notes

## [0.2.0] - 2026-11-18

### Added
- Full alpha transparency support when opening, editing, resizing, and saving PNG and WebP images with a checkerboard canvas backdrop
- Transparency tool (Brush, Fill, and Clear to Transparent)
- Shapes tool with outline and filled modes and `Shift` proportion snapping
- Line tool with live preview and 45-degree `Shift` angle snapping
- Quick Clear Canvas action in the floating toolbar
- Debian packaging metadata

## [0.1.1] - 2026-11-18

### Changed
- Improved large-canvas drawing and zoom performance without tile seams up to 3200% zoom
- Added two-finger touchscreen pinch-to-zoom and pan support
- Added cursor-anchored zooming and smoother low-zoom step transitions

## [0.1.0] - 2026-11-17

### Added
- Initial release with tabbed canvases, Brush, Eraser, Flood Fill, automatic session restore, and Adwaita preferences
