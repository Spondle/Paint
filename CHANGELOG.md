# Changelog

All notable changes to Paint will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Native OpenRaster (`.ora`) multi-layer project reading and writing via pure-C ZIP & zlib streams, preserving layer names, visibility, opacity, blend modes, and locks
- Dedicated **Export Image…** dialog (`Ctrl+Shift+E`) with PNG, JPEG (configurable quality & matte color), WebP (optional `libwebp`), and BMP support, plus flatten confirmation warnings when saving multi-layer projects to flat formats
- Layer **Alpha Lock**, **Layer Lock**, drag-and-drop layer reordering, layer row context menu, **Solo Layer** (`Alt+Click` visibility eye), **Import Image as Layer…**, **Copy Merged** (`Ctrl+Shift+C`), **Paste as New Layer** (`Ctrl+Alt+V`), sidebar file drops, and layer-vs-tab drop confirmation on non-blank canvases
- **Ellipse Select**, **Freehand Lasso**, and **Magic Wand** selection tools, **Invert Selection** (`Ctrl+Shift+I`), and 8 interactive selection resize handles with `Shift` aspect ratio locking
- Flood Fill **Tolerance** slider (`0–255`) and **Sample All Layers** option
- **Gradient** tool (`G`, Linear & Radial) plus **Blur** and **Pixelate** retouch brushes
- Stylus pressure sensitivity (`GDK_AXIS_PRESSURE` controlling brush size and/or opacity in **Preferences → Stylus**) and hardware tablet eraser routing
- Multiline Text overlay (`GtkTextView`), `GtkFontDialogButton` font chooser, Left/Center/Right text alignment, Secondary-color **Outline**, and **Fill** pill backdrop
- Overlapping **Primary / Secondary** color swatches (`Shift+X` swap, `D` B&W reset), persisted 16-slot **Custom Palette**, and persisted **Recent Colors** across sessions
- Image Adjustments & Filters menu with live canvas preview (**Brightness & Contrast…**, **Hue & Saturation…**, **Gaussian Blur…**), one-shot **Sharpen**, **Invert Colors**, and Rec. 709 **Grayscale** (all selection-mask and Alpha Lock aware), plus **Trim Transparent**
- Bottom-left live cursor coordinates & selection/canvas dimensions HUD pill, draggable East/South/SE canvas edge resize handles, Spacebar-drag canvas panning, right-click canvas context menu, and **Print…** (`Ctrl+P` via `GtkPrintOperation`)
- `gettext` internationalization scaffolding (`po/POTFILES`, `po/LINGUAS`, `src/paint-i18n.h`) and wrapped UI strings

### Changed
- Modularized codebase into focused C translation units (`paint-document`, `paint-io`, `paint-filters`, `paint-color-picker`, `paint-layers-panel`, `paint-session`, `paint-dialogs`)
- Upgraded undo/redo history from a fixed step limit to a `256 MB` memory budget (`10–250` steps) with oldest-step eviction
- Migrated all file load/save routines to GIO streams and added atomic session saves with 30-second background autosave

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
